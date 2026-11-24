package megler;

import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentProviderClient;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.OperationApplicationException;
import android.content.ServiceConnection;
import android.content.pm.ProviderInfo;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.HashMap;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Per-connection service bindings; the owner closes it when the transport dies. */
public final class SharedMicrogServiceRouter implements AutoCloseable {
    public static final int BIND = 1;
    public static final int UNBIND = 2;
    public static final int ACCOUNTS = 3;
    public static final int BIND_INTENT = 4;
    public static final int START_SERVICE = 5;
    public static final int STOP_SERVICE = 6;
    public static final int SEND_BROADCAST = 7;
    public static final int REGISTER_CLIENT_CALLBACK = 8;
    public static final int UNREGISTER_CLIENT_CALLBACK = 9;
    public static final int BROADCAST_FROM_WORKER = 10;
    public static final int PROVIDER_EXISTS = 11;
    public static final int PROVIDER_GET_TYPE = 12;
    public static final int PROVIDER_QUERY = 13;
    public static final int PROVIDER_CALL = 14;
    public static final int PROVIDER_INSERT = 15;
    public static final int PROVIDER_UPDATE = 16;
    public static final int PROVIDER_DELETE = 17;
    public static final int PROVIDER_GET_STREAM_TYPES = 18;
    public static final int PROVIDER_CANONICALIZE = 19;
    public static final int PROVIDER_UNCANONICALIZE = 20;
    public static final int PROVIDER_REFRESH = 21;
    public static final int PROVIDER_OPEN_FILE = 22;
    public static final int PROVIDER_OPEN_ASSET = 23;
    public static final int PROVIDER_OPEN_TYPED_ASSET = 24;
    public static final int PROVIDER_READ_STREAM = 25;
    public static final int PROVIDER_CLOSE_STREAM = 26;
    public static final int PROVIDER_APPLY_BATCH = 27;
    public static final int PROVIDER_REGISTER_OBSERVER = 28;
    public static final int PROVIDER_UNREGISTER_OBSERVER = 29;
    public static final int PROVIDER_OBSERVER_CHANGED = 30;
    public static final int PROVIDER_OPEN_WRITE_STREAM = 31;
    public static final int PROVIDER_WRITE_STREAM = 32;
    public static final int PROVIDER_FINISH_WRITE_STREAM = 33;
    public static final int PROVIDER_NOTIFY_CHANGE = 34;
    public static final int SEND_ORDERED_BROADCAST = 35;
    public static final int ORDERED_BROADCAST_RESULT = 36;
    public static final int ORDERED_BROADCAST_FROM_WORKER = 37;
    public static final int ORDERED_BROADCAST_CLIENT_RESULT = 38;
    private static final String PACKAGE = "com.google.android.gms";
    private static final int MAX_BINDINGS = 128;
    private static final int MAX_PROVIDER_STREAMS = 32;
    private static final int PROVIDER_CHUNK_SIZE = 32768;
    private final Context context;
    private final Resolver resolver;
    private final ProviderResolver providerResolver;
    private final SharedMicrogAccounts.Server accountServer;
    private final HashMap<Integer, Binding> bindings = new HashMap<>();
    private final HashMap<Integer, ProviderStream> providerStreams = new HashMap<>();
    private final HashMap<Integer, ContentObserver> providerObservers = new HashMap<>();
    private final java.util.HashSet<Integer> orderedBroadcasts = new java.util.HashSet<>();
    private final HashMap<Integer, OrderedCompletion> clientOrderedBroadcasts = new HashMap<>();
    private int nextClientOrderedId = 1;
    private final OrderedDispatcher orderedDispatcher;
    private IBinder clientBroadcast;
    private volatile boolean closed;
    private int nextProviderStream = 1;

    interface Resolver { ServiceInfo resolve(Intent intent); }
    interface OrderedCompletion { void finish(int code, String data, Bundle extras); }
    interface OrderedDispatcher {
        void dispatch(Intent intent, int code, String data, Bundle extras, OrderedCompletion completion);
    }
    interface ProviderResolver {
        ProviderInfo resolve(String authority);
        ContentProvider provider(String authority);
    }

    private static final class ProviderStream {
        final InputStream input;
        final OutputStream output;
        private final java.util.concurrent.atomic.AtomicBoolean closed = new java.util.concurrent.atomic.AtomicBoolean();
        ProviderStream(InputStream input, OutputStream output) { this.input = input; this.output = output; }
        byte[] read() throws IOException {
            if (closed.get() || input == null) throw new IOException("Provider stream is not readable");
            byte[] data = new byte[PROVIDER_CHUNK_SIZE];
            int count;
            do { count = input.read(data); } while (count == 0);
            return count < 0 ? null : java.util.Arrays.copyOf(data, count);
        }
        synchronized void write(byte[] chunk) throws IOException {
            if (closed.get() || output == null) throw new IOException("Provider stream is not writable");
            output.write(chunk);
        }
        void close() throws IOException {
            if (!closed.compareAndSet(false, true)) return;
            if (input != null) input.close();
            if (output != null) output.close();
        }
    }

    private static final class OpenProviderStream {
        final int id;
        final long length;
        OpenProviderStream(int id, long length) { this.id = id; this.length = length; }
    }

    private static final class Binding implements ServiceConnection {
        final CountDownLatch ready = new CountDownLatch(1);
        volatile IBinder binder;
        volatile ComponentName name;
        boolean registered;
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            this.name = name;
            this.binder = binder;
            ready.countDown();
        }
        @Override public void onNullBinding(ComponentName name) {
            this.name = name;
            ready.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            ready.countDown();
        }
    }

    public SharedMicrogServiceRouter(Context context) {
        this(context, intent -> {
            ResolveInfo result = context.getPackageManager().resolveService(intent, 0);
            return result == null ? null : result.serviceInfo;
        }, null);
    }

    SharedMicrogServiceRouter(Context context, Resolver resolver) {
        this(context, resolver, null);
    }

    SharedMicrogServiceRouter(Context context, SharedMicrogAccounts.Server accountServer) {
        this(context, intent -> {
            ResolveInfo result = context.getPackageManager().resolveService(intent, 0);
            return result == null ? null : result.serviceInfo;
        }, accountServer);
    }

    SharedMicrogServiceRouter(Context context, Resolver resolver, SharedMicrogAccounts.Server accountServer) {
        this(context, resolver, accountServer, new ProviderResolver() {
            @Override public ProviderInfo resolve(String authority) {
                return context.getPackageManager().resolveContentProvider(authority, 0);
            }
            @Override public ContentProvider provider(String authority) {
                ContentProviderClient client = context.getContentResolver().acquireContentProviderClient(authority);
                return client == null ? null : client.getLocalContentProvider();
            }
        });
    }

    SharedMicrogServiceRouter(Context context, Resolver resolver, SharedMicrogAccounts.Server accountServer,
                              ProviderResolver providerResolver) {
        this(context, resolver, accountServer, providerResolver, Broadcasts::sendOrderedFromSharedClient);
    }

    SharedMicrogServiceRouter(Context context, Resolver resolver, SharedMicrogAccounts.Server accountServer,
                              ProviderResolver providerResolver, OrderedDispatcher orderedDispatcher) {
        if (context == null) throw new IllegalArgumentException("Context is required");
        if (resolver == null) throw new IllegalArgumentException("Service resolver is required");
        if (providerResolver == null) throw new IllegalArgumentException("Provider resolver is required");
        if (orderedDispatcher == null) throw new IllegalArgumentException("Ordered dispatcher is required");
        this.context = context;
        this.resolver = resolver;
        this.providerResolver = providerResolver;
        this.accountServer = accountServer;
        this.orderedDispatcher = orderedDispatcher;
    }

    private static void rejectProtectedClientBroadcast(Intent intent) {
        if (intent != null && SharedMicrogProtectedBroadcasts.contains(intent.getAction()))
            throw new SecurityException("Client cannot send protected broadcasts to shared microG.");
    }

    private final Binder root = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == ORDERED_BROADCAST_CLIENT_RESULT) {
                if (flags != IBinder.FLAG_ONEWAY || reply != null)
                    throw new RemoteException("One-way ordered result required");
                int id = data.readInt(), status = data.readInt(), resultCode = data.readInt();
                String resultData = data.readString();
                Bundle extras = data.readBundle();
                requireConsumed(data);
                if (id <= 0 || (status != 0 && status != 1) ||
                        (resultData != null && resultData.length() > 8192) || (extras != null && extras.size() > 128))
                    throw new RemoteException("Invalid client ordered broadcast result");
                OrderedCompletion completion;
                synchronized (bindings) { completion = clientOrderedBroadcasts.remove(id); }
                if (completion == null) return true;
                if (status != 0) System.err.println("Shared microG client ordered broadcast failed.");
                else completion.finish(resultCode, resultData, extras);
                return true;
            }
            if (flags != 0 || reply == null) throw new RemoteException("Synchronous request required");
            if (code == REGISTER_CLIENT_CALLBACK) {
                IBinder callback = data.readStrongBinder();
                if (callback == null || data.dataAvail() != 0 || !callback.isBinderAlive())
                    throw new RemoteException("Invalid shared microG client callback");
                synchronized (bindings) {
                    if (closed) throw new RemoteException("Shared microG service router is closed");
                    if (clientBroadcast != null) throw new RemoteException("Shared microG client callback already registered");
                    clientBroadcast = callback;
                }
                reply.writeNoException();
                return true;
            }
            if (code == UNREGISTER_CLIENT_CALLBACK) {
                if (data.dataAvail() != 0) throw new RemoteException("Invalid callback removal request");
                synchronized (bindings) {
                    clientBroadcast = null;
                    orderedBroadcasts.clear();
                    clientOrderedBroadcasts.clear();
                }
                reply.writeNoException();
                return true;
            }
            if (code == ACCOUNTS) {
                synchronized (bindings) {
                    if (closed || accountServer == null) throw new RemoteException("Shared account service unavailable");
                    reply.writeNoException();
                    reply.writeStrongBinder(accountServer.binder());
                }
                return true;
            }
            if (code == BIND) {
                int id = data.readInt();
                String action = data.readString();
                String component = data.readString();
                if (data.dataAvail() != 0) throw new RemoteException("Invalid legacy bind request");
                Binding binding = bind(id, action, component);
                reply.writeNoException();
                reply.writeString(binding.name == null ? null : binding.name.getClassName());
                reply.writeStrongBinder(binding.binder);
                return true;
            }
            if (code == BIND_INTENT) {
                int id = data.readInt();
                Binding binding = bind(id, readIntent(data));
                reply.writeNoException();
                reply.writeString(binding.name == null ? null : binding.name.getClassName());
                reply.writeStrongBinder(binding.binder);
                return true;
            }
            if (code == START_SERVICE) {
                ComponentName name = startService(readIntent(data));
                reply.writeNoException();
                reply.writeString(name == null ? null : name.getClassName());
                return true;
            }
            if (code == STOP_SERVICE) {
                boolean stopped = stopService(readIntent(data));
                reply.writeNoException();
                reply.writeInt(stopped ? 1 : 0);
                return true;
            }
            if (code == SEND_BROADCAST) {
                Intent intent = readIntent(data);
                rejectProtectedClientBroadcast(intent);
                Broadcasts.sendFromSharedClient(intent);
                reply.writeNoException();
                return true;
            }
            if (code == SEND_ORDERED_BROADCAST) {
                int id = data.readInt();
                Intent intent = data.readParcelable(Intent.class.getClassLoader());
                int resultCode = data.readInt();
                String resultData = data.readString();
                Bundle extras = data.readBundle();
                requireConsumed(data);
                if (id <= 0 || !SharedMicrogClientRouter.canRouteOrdered(intent) ||
                        (resultData != null && resultData.length() > 8192) || (extras != null && extras.size() > 128))
                    throw new RemoteException("Invalid shared microG ordered broadcast");
                rejectProtectedClientBroadcast(intent);
                synchronized (bindings) {
                    if (closed || clientBroadcast == null || orderedBroadcasts.size() >= 128 ||
                            !orderedBroadcasts.add(id))
                        throw new RemoteException("Shared microG ordered broadcast registry unavailable");
                }

                try {
                    orderedDispatcher.dispatch(intent, resultCode, resultData, extras,
                        (result, text, resultExtras) -> finishOrderedBroadcast(id, result, text, resultExtras));
                } catch (RuntimeException e) {
                    synchronized (bindings) { orderedBroadcasts.remove(id); }
                    throw e;
                }
                reply.writeNoException();
                return true;
            }
            if (code >= PROVIDER_EXISTS && code <= PROVIDER_NOTIFY_CHANGE &&
                    code != PROVIDER_OBSERVER_CHANGED) {
                handleProviderRequest(code, data, reply);
                return true;
            }
            if (code == UNBIND) {
                int id = data.readInt();
                if (data.dataAvail() != 0) throw new RemoteException("Invalid unbind request");
                unbind(id);
                reply.writeNoException();
                return true;
            }
            return false;
        }
    };

    public IBinder root() { return root; }

    private void finishOrderedBroadcast(int id, int code, String text, Bundle extras) {
        IBinder callback;
        synchronized (bindings) {
            if (!orderedBroadcasts.remove(id) || closed) return;
            callback = clientBroadcast;
        }
        if (callback == null || !callback.isBinderAlive()) {
            System.err.println("Shared microG ordered broadcast result lost: client disconnected.");
            return;
        }
        Parcel result = Parcel.obtain();
        try {
            result.writeInt(id);
            result.writeInt(0);
            result.writeInt(code);
            result.writeString(text);
            result.writeBundle(extras);
            if ((text != null && text.length() > 8192) || (extras != null && extras.size() > 128))
                throw new IllegalArgumentException("Ordered broadcast result exceeds transfer limits");
            if (!callback.transact(ORDERED_BROADCAST_RESULT, result, null, IBinder.FLAG_ONEWAY))
                throw new RemoteException("Shared microG ordered broadcast result rejected");
        } catch (RemoteException | RuntimeException e) {
            System.err.println("Shared microG ordered broadcast result failed: " + e);
            Parcel failure = Parcel.obtain();
            try {
                failure.writeInt(id);
                failure.writeInt(1);
                failure.writeInt(0);
                failure.writeString(null);
                failure.writeBundle(null);
                if (!callback.transact(ORDERED_BROADCAST_RESULT, failure, null, IBinder.FLAG_ONEWAY))
                    throw new RemoteException("Shared microG ordered broadcast failure rejected");
            } catch (RemoteException | RuntimeException failureError) {
                System.err.println("Shared microG ordered broadcast failure notification failed: " + failureError);
            } finally { failure.recycle(); }
        } finally { result.recycle(); }
    }

    private void handleProviderRequest(int code, Parcel data, Parcel reply) throws RemoteException {
        if (code == PROVIDER_EXISTS) {
            String authority = readAuthority(data);
            requireConsumed(data);
            reply.writeNoException();
            reply.writeInt(providerInfo(authority) != null ? 1 : 0);
            return;
        }
        if (code == PROVIDER_READ_STREAM) {
            int id = data.readInt();
            requireConsumed(data);
            byte[] chunk = readProviderStream(id);
            reply.writeNoException();
            reply.writeByteArray(chunk);
            return;
        }
        if (code == PROVIDER_CLOSE_STREAM) {
            int id = data.readInt();
            requireConsumed(data);
            closeProviderStream(id);
            reply.writeNoException();
            return;
        }
        if (code == PROVIDER_WRITE_STREAM) {
            int id = data.readInt();
            byte[] chunk = data.createByteArray();
            requireConsumed(data);
            if (chunk == null || chunk.length == 0 || chunk.length > PROVIDER_CHUNK_SIZE)
                throw new RemoteException("Invalid provider write chunk");
            ProviderStream stream;
            synchronized (providerStreams) { stream = providerStreams.get(id); }
            if (stream == null) throw new RemoteException("Unknown shared microG provider stream");
            try { stream.write(chunk); }
            catch (IOException e) {
                closeProviderStream(id);
                throw new RemoteException("Shared microG provider stream write failed");
            }
            reply.writeNoException();
            return;
        }
        if (code == PROVIDER_FINISH_WRITE_STREAM) {
            int id = data.readInt();
            requireConsumed(data);
            ProviderStream stream;
            synchronized (providerStreams) {
                stream = providerStreams.get(id);
                if (stream == null || stream.output == null)
                    throw new RemoteException("Unknown shared microG provider write stream");
                providerStreams.remove(id);
            }
            try { stream.close(); }
            catch (IOException e) { throw new RemoteException("Shared microG provider stream close failed"); }
            reply.writeNoException();
            return;
        }
        if (code == PROVIDER_UNREGISTER_OBSERVER) {
            int id = data.readInt();
            requireConsumed(data);
            synchronized (bindings) {
                ContentObserver observer = providerObservers.remove(id);
                if (observer != null) context.getContentResolver().meglerUnregisterLocalObserver(observer);
            }
            reply.writeNoException();
            return;
        }

        String authority = readAuthority(data);
        if (code == PROVIDER_NOTIFY_CHANGE) {
            Uri uri = readProviderUri(data, authority);
            int count = data.readInt();
            if (count < 0 || count > 128) throw new RemoteException("Invalid provider notification exclusions");
            ArrayList<ContentObserver> excluded = new ArrayList<>();
            synchronized (bindings) {
                for (int i = 0; i < count; i++) {
                    ContentObserver observer = providerObservers.get(data.readInt());
                    if (observer == null) throw new RemoteException("Unknown provider observer");
                    excluded.add(observer);
                }
            }
            requireConsumed(data);
            publicProvider(authority);
            context.getContentResolver().meglerNotifyLocalChangeExcluding(uri, excluded);
            reply.writeNoException();
            return;
        }
        if (code == PROVIDER_OPEN_WRITE_STREAM) {
            Uri uri = readProviderUri(data, authority);
            String mode = readBoundedString(data, 4);
            requireConsumed(data);
            if (!"w".equals(mode) && !"wt".equals(mode) && !"wa".equals(mode))
                throw new RemoteException("Unsupported shared microG provider write mode");
            try {
                android.os.ParcelFileDescriptor descriptor = publicProvider(authority).openFile(uri, mode);
                if (descriptor == null) throw new RemoteException("Provider returned no write descriptor");
                int id = addProviderStream(null, new android.os.ParcelFileDescriptor.AutoCloseOutputStream(descriptor));
                reply.writeNoException();
                reply.writeInt(id);
            } catch (IOException e) {
                throw new RemoteException("Shared microG provider write open failed");
            }
            return;
        }
        if (code == PROVIDER_REGISTER_OBSERVER) {
            Uri uri = readProviderUri(data, authority);
            int descendants = data.readInt(), id = data.readInt();
            requireConsumed(data);
            if (id <= 0 || (descendants != 0 && descendants != 1))
                throw new RemoteException("Invalid provider observer request");
            publicProvider(authority);
            ContentObserver observer = new ContentObserver(null) {
                @Override public void onChange(boolean self, Uri changed) {
                    if (changed == null || !matchesObserverUri(uri, changed, descendants != 0)) return;
                    try {
                        if (providerInfo(authority) == null) return;
                    } catch (SecurityException e) {
                        System.err.println("Shared microG provider observer access revoked.");
                        return;
                    }
                    IBinder callback;
                    synchronized (bindings) {
                        if (closed || providerObservers.get(id) != this) return;
                        callback = clientBroadcast;
                    }
                    if (callback == null || !callback.isBinderAlive()) return;
                    Parcel event = Parcel.obtain();
                    try {
                        event.writeInt(id);
                        event.writeString(changed.toString());
                        if (!callback.transact(PROVIDER_OBSERVER_CHANGED, event, null, IBinder.FLAG_ONEWAY))
                            throw new RemoteException("Shared microG observer callback rejected");
                    } catch (RemoteException | RuntimeException e) {
                        System.err.println("Shared microG observer callback failed: " + e);
                    } finally { event.recycle(); }
                }
            };
            synchronized (bindings) {
                if (closed || clientBroadcast == null || providerObservers.size() >= 128 ||
                        providerObservers.containsKey(id))
                    throw new RemoteException("Shared microG observer registration unavailable");
                context.getContentResolver().meglerRegisterLocalObserver(uri, descendants != 0, observer);
                providerObservers.put(id, observer);
            }
            reply.writeNoException();
            return;
        }
        if (code == PROVIDER_APPLY_BATCH) {
            int count = data.readInt();
            if (count < 0 || count > 128) throw new RemoteException("Invalid provider batch size");
            ArrayList<ContentProviderOperation> operations = new ArrayList<>(count);
            for (int i = 0; i < count; i++) {
                ContentProviderOperation operation = data.readTypedObject(ContentProviderOperation.CREATOR);
                if (operation == null || operation.getUri() == null ||
                        !"content".equals(operation.getUri().getScheme()) ||
                        !authority.equals(operation.getUri().getAuthority()))
                    throw new RemoteException("Provider batch operation authority mismatch");
                operations.add(operation);
            }

            requireConsumed(data);
            ContentProvider provider = publicProvider(authority);
            try {
                ContentProviderResult[] results = provider.applyBatch(authority, operations);
                if (results == null || results.length != count)
                    throw new RemoteException("Provider returned an invalid batch result");
                reply.writeNoException();
                reply.writeInt(results.length);
                for (ContentProviderResult result : results) {
                    if (result == null) throw new RemoteException("Provider returned an empty batch result");
                    reply.writeTypedObject(result, 0);
                }
            } catch (OperationApplicationException e) {
                reply.writeNoException();
                reply.writeInt(-1);
                String message = e.getMessage();
                reply.writeString(message == null ? null : message.length() > 4096 ? message.substring(0, 4096) : message);
            }
            return;
        }
        if (code == PROVIDER_OPEN_FILE || code == PROVIDER_OPEN_ASSET) {
            Uri uri = readProviderUri(data, authority);
            String mode = readBoundedString(data, 4);
            if (!"r".equals(mode)) throw new RemoteException("Shared microG providers are read-only over IPC");
            requireConsumed(data);
            ContentProvider provider = publicProvider(authority);
            try {
                if (code == PROVIDER_OPEN_FILE) {
                    android.os.ParcelFileDescriptor descriptor = provider.openFile(uri, mode);
                    writeOpenedProviderStream(reply, descriptor == null ? null :
                        new android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor), -1);
                } else {
                    AssetFileDescriptor descriptor = provider.openAssetFile(uri, mode);
                    writeOpenedProviderStream(reply, descriptor == null ? null : descriptor.createInputStream(),
                        descriptor == null ? -1 : descriptor.getLength());
                }
            } catch (IOException e) {
                throw new RemoteException("Shared microG provider file open failed");
            }
            return;
        }
        if (code == PROVIDER_OPEN_TYPED_ASSET) {
            Uri uri = readProviderUri(data, authority);
            String mimeType = readBoundedString(data, 4096);
            Bundle options = data.readBundle(Bundle.class.getClassLoader());
            requireConsumed(data);
            try {
                AssetFileDescriptor descriptor = publicProvider(authority).openTypedAssetFile(uri, mimeType, options);
                writeOpenedProviderStream(reply, descriptor == null ? null : descriptor.createInputStream(),
                    descriptor == null ? -1 : descriptor.getLength());
            } catch (IOException e) {
                throw new RemoteException("Shared microG typed provider file open failed");
            }
            return;
        }

        Uri uri = readProviderUri(data, authority);
        ContentProvider provider = publicProvider(authority);
        if (code == PROVIDER_GET_TYPE) {
            requireConsumed(data);
            reply.writeNoException();
            reply.writeString(provider.getType(uri));
        } else if (code == PROVIDER_QUERY) {
            int rawBundleQuery = data.readInt();
            if (rawBundleQuery != 0 && rawBundleQuery != 1)
                throw new RemoteException("Invalid provider query mode");
            boolean bundleQuery = rawBundleQuery != 0;
            String[] projection = readStringArray(data, 128, 4096);
            Cursor cursor;
            if (bundleQuery) {
                Bundle args = data.readBundle(Bundle.class.getClassLoader());
                requireConsumed(data);
                cursor = provider.query(uri, projection, args, null);
            } else {
                String selection = readBoundedString(data, 8192);
                String[] selectionArgs = readStringArray(data, 128, 4096);
                String sortOrder = readBoundedString(data, 8192);
                requireConsumed(data);
                cursor = provider.query(uri, projection, selection, selectionArgs, sortOrder);
            }
            reply.writeNoException();
            writeCursor(reply, cursor);
        } else if (code == PROVIDER_CALL) {
            String method = readBoundedString(data, 4096);
            String arg = readBoundedString(data, 4096);
            Bundle extras = data.readBundle(Bundle.class.getClassLoader());
            requireConsumed(data);
            Bundle result = provider.call(authority, method, arg, extras);
            reply.writeNoException();
            reply.writeBundle(result);
        } else if (code == PROVIDER_INSERT) {
            ContentValues values = readContentValues(data);
            requireConsumed(data);
            Uri result = provider.insert(uri, values);
            reply.writeNoException();
            reply.writeString(result == null ? null : result.toString());
        } else if (code == PROVIDER_UPDATE) {
            ContentValues values = readContentValues(data);
            String selection = readBoundedString(data, 8192);
            String[] selectionArgs = readStringArray(data, 128, 4096);
            requireConsumed(data);
            reply.writeNoException();
            reply.writeInt(provider.update(uri, values, selection, selectionArgs));
        } else if (code == PROVIDER_DELETE) {
            String selection = readBoundedString(data, 8192);
            String[] selectionArgs = readStringArray(data, 128, 4096);
            requireConsumed(data);
            reply.writeNoException();
            reply.writeInt(provider.delete(uri, selection, selectionArgs));
        } else if (code == PROVIDER_GET_STREAM_TYPES) {
            String filter = readBoundedString(data, 4096);
            requireConsumed(data);
            String[] types = provider.getStreamTypes(uri, filter);
            if (types != null && types.length > 128) throw new RemoteException("Too many provider stream types");
            reply.writeNoException();
            reply.writeStringArray(types);
        } else if (code == PROVIDER_CANONICALIZE || code == PROVIDER_UNCANONICALIZE) {
            requireConsumed(data);
            Uri result = code == PROVIDER_CANONICALIZE ? provider.canonicalize(uri) : provider.uncanonicalize(uri);
            reply.writeNoException();
            reply.writeString(result == null ? null : result.toString());
        } else if (code == PROVIDER_REFRESH) {
            Bundle extras = data.readBundle(Bundle.class.getClassLoader());
            requireConsumed(data);
            reply.writeNoException();
            reply.writeInt(provider.refresh(uri, extras, null) ? 1 : 0);
        } else {
            throw new RemoteException("Unknown shared microG provider transaction");
        }
    }

    static boolean matchesObserverUri(Uri registered, Uri changed, boolean descendants) {
        if (!"content".equals(changed.getScheme()) ||
                !registered.getAuthority().equals(changed.getAuthority()) ||
                changed.toString().length() > 8192) return false;
        java.util.List<String> a = registered.getPathSegments(), b = changed.getPathSegments();
        if (b.size() < a.size() || (!descendants && b.size() != a.size())) return false;
        for (int i = 0; i < a.size(); i++) if (!a.get(i).equals(b.get(i))) return false;
        return true;
    }

    private ProviderInfo providerInfo(String authority) {
        validateAuthority(authority);
        ProviderInfo info = providerResolver.resolve(authority);
        if (info == null) return null;
        if (!PACKAGE.equals(info.packageName) || !info.enabled || !info.exported ||
                hasPermission(info.readPermission) || hasPermission(info.writePermission) ||
                info.pathPermissions != null && info.pathPermissions.length != 0 ||
                info.grantUriPermissions || info.forceUriPermissions ||
                info.uriPermissionPatterns != null && info.uriPermissionPatterns.length != 0 ||
                !hasAuthority(info.authority, authority))
            throw new SecurityException("Shared microG provider is not public");
        return info;
    }

    private ContentProvider publicProvider(String authority) {
        if (providerInfo(authority) == null)
            throw new IllegalArgumentException("Shared microG provider is unavailable");
        ContentProvider provider = providerResolver.provider(authority);
        if (provider == null) throw new IllegalStateException("Shared microG provider is not registered");
        String actual = provider.getAuthority();
        if (actual != null && !hasAuthority(actual, authority))
            throw new SecurityException("Shared microG provider authority mismatch");
        return provider;
    }

    private static boolean hasAuthority(String authorities, String authority) {
        if (authorities == null) return false;
        for (String value : authorities.split(";"))
            if (authority.equals(value.trim())) return true;
        return false;
    }

    private static boolean hasPermission(String permission) {
        return permission != null && !permission.isEmpty();
    }

    private static void validateAuthority(String authority) {
        if (authority == null || authority.isEmpty() || authority.length() > 4096 ||
                authority.indexOf(';') >= 0)
            throw new IllegalArgumentException("Invalid shared microG provider authority");
    }

    private static String readAuthority(Parcel data) throws RemoteException {
        String authority = data.readString();
        try { validateAuthority(authority); }
        catch (RuntimeException e) { throw new RemoteException("Invalid shared microG provider authority"); }
        return authority;
    }

    private static Uri readProviderUri(Parcel data, String authority) throws RemoteException {
        String value = readBoundedString(data, 8192);
        if (value == null) throw new RemoteException("Provider URI is required");
        Uri uri;
        try { uri = Uri.parse(value); }
        catch (RuntimeException e) { throw new RemoteException("Invalid provider URI"); }
        if (!"content".equals(uri.getScheme()) || !authority.equals(uri.getAuthority()))
            throw new RemoteException("Provider URI authority mismatch");
        return uri;
    }

    private static String readBoundedString(Parcel data, int maxLength) throws RemoteException {
        String value = data.readString();
        if (value != null && value.length() > maxLength) throw new RemoteException("Provider string exceeds limit");
        return value;
    }

    private static String[] readStringArray(Parcel data, int maxItems, int maxLength) throws RemoteException {
        String[] values = data.createStringArray();
        if (values == null) return null;
        if (values.length > maxItems) throw new RemoteException("Provider string array exceeds limit");
        for (String value : values)
            if (value != null && value.length() > maxLength)
                throw new RemoteException("Provider string array item exceeds limit");
        return values;
    }

    private static void requireConsumed(Parcel data) throws RemoteException {
        if (data.dataAvail() != 0) throw new RemoteException("Unexpected shared microG provider request data");
    }

    static void writeContentValues(Parcel data, ContentValues values) {
        if (values == null) { data.writeInt(-1); return; }
        if (values.size() > 128) throw new IllegalArgumentException("Too many provider values");
        data.writeInt(values.size());
        for (java.util.Map.Entry<String, Object> entry : values.valueSet()) {
            String key = entry.getKey();
            Object value = entry.getValue();
            if (key == null || key.isEmpty() || key.length() > 1024)
                throw new IllegalArgumentException("Invalid provider value key");
            data.writeString(key);
            if (value == null) data.writeInt(0);
            else if (value instanceof String) {
                if (((String) value).length() > 65536) throw new IllegalArgumentException("Provider string value exceeds limit");
                data.writeInt(1); data.writeString((String) value);
            } else if (value instanceof Boolean) { data.writeInt(2); data.writeInt((Boolean) value ? 1 : 0); }
            else if (value instanceof Byte) { data.writeInt(3); data.writeInt((Byte) value); }
            else if (value instanceof Short) { data.writeInt(4); data.writeInt((Short) value); }
            else if (value instanceof Integer) { data.writeInt(5); data.writeInt((Integer) value); }
            else if (value instanceof Long) { data.writeInt(6); data.writeLong((Long) value); }
            else if (value instanceof Float) { data.writeInt(7); data.writeFloat((Float) value); }
            else if (value instanceof Double) { data.writeInt(8); data.writeDouble((Double) value); }
            else if (value instanceof byte[]) {
                if (((byte[]) value).length > 262144) throw new IllegalArgumentException("Provider blob exceeds limit");
                data.writeInt(9); data.writeByteArray((byte[]) value);
            } else throw new IllegalArgumentException("Unsupported provider value type");
        }
    }

    static ContentValues readContentValues(Parcel data) throws RemoteException {
        int count = data.readInt();
        if (count < 0) return null;
        if (count > 128) throw new RemoteException("Too many provider values");
        ContentValues values = new ContentValues(count);
        for (int i = 0; i < count; i++) {
            String key = data.readString();
            if (key == null || key.isEmpty() || key.length() > 1024)
                throw new RemoteException("Invalid provider value key");
            Object value;
            switch (data.readInt()) {
                case 0: value = null; break;
                case 1: value = readBoundedString(data, 65536); break;
                case 2: {
                    int bool = data.readInt();
                    if (bool != 0 && bool != 1) throw new RemoteException("Invalid provider boolean value");
                    value = bool != 0;
                    break;
                }
                case 3: value = (byte) data.readInt(); break;
                case 4: value = (short) data.readInt(); break;
                case 5: value = data.readInt(); break;
                case 6: value = data.readLong(); break;
                case 7: value = data.readFloat(); break;
                case 8: value = data.readDouble(); break;
                case 9:
                    value = data.createByteArray();
                    if (value == null || ((byte[]) value).length > 262144)
                        throw new RemoteException("Invalid provider blob");
                    break;
                default: throw new RemoteException("Unsupported provider value type");
            }
            values.putObject(key, value);
        }
        return values;
    }

    private static void writeCursor(Parcel reply, Cursor cursor) throws RemoteException {
        if (cursor == null) { reply.writeInt(-1); return; }
        int total = 0;
        try {
            String[] columns = cursor.getColumnNames();
            if (columns == null || columns.length > 128) throw new RemoteException("Provider cursor has too many columns");
            reply.writeInt(columns.length);
            for (String column : columns) {
                if (column == null || column.length() > 4096) throw new RemoteException("Invalid provider cursor column");
                total += column.length() * 2;
                reply.writeString(column);
            }
            int rows = cursor.getCount();
            if (rows < 0 || rows > 1024) throw new RemoteException("Provider cursor has too many rows");
            reply.writeInt(rows);
            for (int row = 0; row < rows; row++) {
                if (!cursor.moveToPosition(row)) throw new RemoteException("Provider cursor changed during transfer");
                for (int column = 0; column < columns.length; column++) {
                    int type = cursor.getType(column);
                    reply.writeInt(type);
                    switch (type) {
                        case Cursor.FIELD_TYPE_NULL: break;
                        case Cursor.FIELD_TYPE_INTEGER:
                            reply.writeLong(cursor.getLong(column)); total += 8; break;
                        case Cursor.FIELD_TYPE_FLOAT:
                            reply.writeDouble(cursor.getDouble(column)); total += 8; break;
                        case Cursor.FIELD_TYPE_STRING: {
                            String value = cursor.getString(column);
                            if (value != null && value.length() > 65536) throw new RemoteException("Provider cursor string exceeds limit");
                            total += value == null ? 0 : value.length() * 2;
                            reply.writeString(value);
                            break;
                        }
                        case Cursor.FIELD_TYPE_BLOB: {
                            byte[] value = cursor.getBlob(column);
                            if (value != null && value.length > 262144) throw new RemoteException("Provider cursor blob exceeds limit");
                            total += value == null ? 0 : value.length;
                            reply.writeByteArray(value);
                            break;
                        }
                        default: throw new RemoteException("Unsupported provider cursor value type");
                    }
                    if (total > 524288) throw new RemoteException("Provider cursor exceeds transfer limit");
                }
            }
            Bundle extras = cursor.getExtras();
            if (extras != null && extras.size() > 128) throw new RemoteException("Provider cursor extras exceed limit");
            reply.writeBundle(extras);
            Uri notificationUri = cursor.getNotificationUri();
            reply.writeString(notificationUri == null ? null : notificationUri.toString());
            reply.writeInt(cursor.getWantsAllOnMoveCalls() ? 1 : 0);
        } finally {
            cursor.close();
        }
    }

    private void writeOpenedProviderStream(Parcel reply, InputStream input, long length) throws RemoteException {
        int id = input == null ? -1 : addProviderStream(input, null);
        reply.writeNoException();
        reply.writeInt(id);
        reply.writeLong(length);
    }

    private int addProviderStream(InputStream input, OutputStream output) throws RemoteException {
        synchronized (providerStreams) {
            if (closed || providerStreams.size() >= MAX_PROVIDER_STREAMS) {
                RemoteException failure = new RemoteException("Shared microG provider stream limit reached");
                try { if (input != null) input.close(); else output.close(); }
                catch (IOException closeError) { failure.addSuppressed(closeError); }
                throw failure;
            }
            for (int i = 0; i <= MAX_PROVIDER_STREAMS; i++) {
                int id = nextProviderStream++;
                if (nextProviderStream <= 0) nextProviderStream = 1;
                if (id > 0 && !providerStreams.containsKey(id)) {
                    providerStreams.put(id, new ProviderStream(input, output));
                    return id;
                }
            }
        }
        RemoteException failure = new RemoteException("Shared microG provider stream registry unavailable");
        try { if (input != null) input.close(); else output.close(); }
        catch (IOException closeError) { failure.addSuppressed(closeError); }
        throw failure;
    }

    private byte[] readProviderStream(int id) throws RemoteException {
        ProviderStream stream;
        synchronized (providerStreams) { stream = providerStreams.get(id); }
        if (stream == null) throw new RemoteException("Unknown shared microG provider stream");
        try {
            byte[] data = stream.read();
            if (data == null) closeProviderStream(id);
            return data;
        } catch (IOException e) {
            closeProviderStream(id);
            throw new RemoteException("Shared microG provider stream read failed");
        }
    }

    private void closeProviderStream(int id) {
        ProviderStream stream;
        synchronized (providerStreams) { stream = providerStreams.remove(id); }
        if (stream != null) {
            try { stream.close(); }
            catch (IOException e) { System.err.println("Shared microG provider stream close failed: " + e); }
        }
    }

    void broadcastToClient(Intent intent, String permission, String also, boolean holdsAlso, int uid,
                           String packageName) {
        if (!SharedMicrogClientRouter.canTransfer(intent) ||
                permission != null && permission.length() > 4096 ||
                also != null && also.length() > 4096 ||
                (uid == -1 && packageName != null) ||
                (uid != -1 && (uid <= 0 || !PACKAGE.equals(packageName))))
            throw new IllegalArgumentException("Invalid shared microG worker broadcast");
        IBinder callback;
        synchronized (bindings) {
            if (closed) throw new IllegalStateException("Shared microG service router is closed");
            callback = clientBroadcast;
        }

        if (callback == null || !callback.isBinderAlive())
            throw new IllegalStateException("Shared microG client broadcast callback is unavailable");
        Parcel data = Parcel.obtain();
        try {
            data.writeParcelable(intent, 0);
            data.writeString(permission);
            data.writeString(also);
            data.writeInt(holdsAlso ? 1 : 0);
            data.writeInt(uid);
            data.writeString(packageName);
            if (!callback.transact(BROADCAST_FROM_WORKER, data, null, IBinder.FLAG_ONEWAY))
                throw new RemoteException("Shared microG client rejected the worker broadcast");
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG worker broadcast failed", e);
        } finally {
            data.recycle();
        }
    }

    void broadcastOrderedToClient(Intent intent, int code, String data, Bundle extras, OrderedCompletion completion) {
        if (!SharedMicrogClientRouter.canTransfer(intent) || intent.getComponent() == null ||
                (data != null && data.length() > 8192) || (extras != null && extras.size() > 128) || completion == null)
            throw new IllegalArgumentException("Invalid shared microG client ordered broadcast");
        IBinder callback;
        int id;
        synchronized (bindings) {
            if (closed || clientBroadcast == null || !clientBroadcast.isBinderAlive() ||
                    clientOrderedBroadcasts.size() >= 128)
                throw new IllegalStateException("Shared microG client ordered broadcast registry unavailable");
            do {
                id = nextClientOrderedId++;
                if (nextClientOrderedId <= 0) nextClientOrderedId = 1;
            } while (clientOrderedBroadcasts.containsKey(id));
            clientOrderedBroadcasts.put(id, completion);
            callback = clientBroadcast;
        }
        Parcel request = Parcel.obtain();
        try {
            request.writeInt(id);
            request.writeParcelable(intent, 0);
            request.writeInt(code);
            request.writeString(data);
            request.writeBundle(extras);
            if (!callback.transact(ORDERED_BROADCAST_FROM_WORKER, request, null, IBinder.FLAG_ONEWAY))
                throw new RemoteException("Shared microG client rejected the ordered broadcast");
        } catch (RemoteException | RuntimeException e) {
            synchronized (bindings) { clientOrderedBroadcasts.remove(id); }
            throw new IllegalStateException("Shared microG client ordered broadcast failed", e);
        } finally { request.recycle(); }
    }

    private static Intent readIntent(Parcel data) throws RemoteException {
        Intent intent = data.readParcelable(Intent.class.getClassLoader());
        if (data.dataAvail() != 0 || !SharedMicrogClientRouter.canRoute(intent))
            throw new RemoteException("Invalid shared microG intent");
        return intent;
    }

    private Binding bind(int id, String action, String component) throws RemoteException {
        if (id <= 0 || (action == null && component == null) ||
            (action != null && (action.isEmpty() || action.length() > 4096)) ||
            (component != null && (component.isEmpty() || component.length() > 4096)))
            throw new RemoteException("Invalid service request");
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new RemoteException("Binding cannot wait on the main thread");
        Intent intent = new Intent(action).setPackage(PACKAGE);
        if (component != null) intent.setComponent(new ComponentName(PACKAGE, component));
        Binding binding = new Binding();
        synchronized (bindings) {
            if (closed || bindings.size() >= MAX_BINDINGS || bindings.containsKey(id))
                throw new RemoteException("Service binding registry unavailable");
            ServiceInfo service = resolver.resolve(intent);
            if (service == null || !PACKAGE.equals(service.packageName) ||
                service.name == null || service.name.isEmpty() || !service.exported || !service.enabled ||
                (service.permission != null && !service.permission.isEmpty()) ||
                (component != null && !component.equals(service.name))) {
                bindings.put(id, binding);
                System.err.println("Shared microG service binding refused: service unavailable or not public");
                return binding;
            }
            intent.setComponent(new ComponentName(PACKAGE, service.name));
            bindings.put(id, binding);
        }
        return startBinding(id, binding, intent);
    }

    private Binding bind(int id, Intent intent) throws RemoteException {
        if (id <= 0 || !SharedMicrogClientRouter.canRoute(intent))
            throw new RemoteException("Invalid service request");
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new RemoteException("Binding cannot wait on the main thread");
        ServiceInfo service = publicService(intent);
        Binding binding = new Binding();
        synchronized (bindings) {
            if (closed || bindings.size() >= MAX_BINDINGS || bindings.containsKey(id))
                throw new RemoteException("Service binding registry unavailable");
            if (service == null) {
                bindings.put(id, binding);
                System.err.println("Shared microG service binding refused: service unavailable or not public");
                return binding;
            }
            intent.setComponent(new ComponentName(PACKAGE, service.name));
            bindings.put(id, binding);
        }
        return startBinding(id, binding, intent);
    }

    private Binding startBinding(int id, Binding binding, Intent intent) throws RemoteException {
        final boolean registered;
        try {
            registered = context.bindService(intent, binding, Context.BIND_AUTO_CREATE);
        } catch (RuntimeException | Error failure) {
            synchronized (bindings) {
                if (bindings.get(id) == binding) bindings.remove(id);
            }
            throw failure;
        }
        boolean removed;
        synchronized (bindings) {
            removed = closed || bindings.get(id) != binding;
            if (!removed) binding.registered = registered;
        }
        if (removed) {
            if (registered) context.unbindService(binding);
            throw new RemoteException("Service binding was closed");
        }
        if (!registered) {
            System.err.println("Shared microG service binding refused: " + intent);
            return binding;
        }
        try {
            if (!binding.ready.await(10, TimeUnit.SECONDS)) {
                release(id, binding);
                throw new RemoteException("Service binding timed out");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            release(id, binding);
            throw new RemoteException("Service binding interrupted");
        }
        synchronized (bindings) {
            if (bindings.get(id) != binding)
                throw new RemoteException("Service binding was closed");
        }
        return binding;
    }

    private ComponentName startService(Intent intent) {
        ServiceInfo service = resolver.resolve(intent);
        if (service == null) return null;
        if (!isPublicService(service, intent))
            throw new SecurityException("Shared microG service is not public");
        intent.setComponent(new ComponentName(PACKAGE, service.name));
        return context.startService(intent);
    }

    private boolean stopService(Intent intent) {
        ServiceInfo service = resolver.resolve(intent);
        if (service == null) return false;
        if (!isPublicService(service, intent))
            throw new SecurityException("Shared microG service is not public");
        intent.setComponent(new ComponentName(PACKAGE, service.name));
        return context.stopService(intent);
    }

    private ServiceInfo publicService(Intent intent) {
        ServiceInfo service = resolver.resolve(intent);
        return isPublicService(service, intent) ? service : null;
    }

    private static boolean isPublicService(ServiceInfo service, Intent intent) {
        ComponentName component = intent.getComponent();
        String requested = component == null ? null : component.getClassName();
        if (service == null || !PACKAGE.equals(service.packageName) ||
            service.name == null || service.name.isEmpty() || !service.exported || !service.enabled ||
            (service.permission != null && !service.permission.isEmpty()) ||
            (requested != null && !requested.equals(service.name)))
            return false;
        return true;
    }

    private void unbind(int id) throws RemoteException {
        Binding binding;
        synchronized (bindings) { binding = bindings.remove(id); }
        if (binding == null) throw new RemoteException("Unknown service binding");
        binding.ready.countDown();
        if (binding.registered) context.unbindService(binding);
    }

    private void release(int id, Binding expected) {
        synchronized (bindings) {
            if (bindings.get(id) != expected) return;
            bindings.remove(id);
        }
        expected.ready.countDown();
        if (expected.registered) context.unbindService(expected);
    }

    @Override public void close() {
        Binding[] pending;
        ContentObserver[] observers;
        synchronized (bindings) {
            if (closed) return;
            closed = true;
            clientBroadcast = null;
            orderedBroadcasts.clear();
            if (!clientOrderedBroadcasts.isEmpty())
                System.err.println("Shared microG channel closed with client ordered broadcasts pending.");
            clientOrderedBroadcasts.clear();
            pending = bindings.values().toArray(new Binding[0]);
            bindings.clear();
            observers = providerObservers.values().toArray(new ContentObserver[0]);
            providerObservers.clear();
        }
        for (ContentObserver observer : observers) context.getContentResolver().meglerUnregisterLocalObserver(observer);
        for (Binding binding : pending) {
            binding.ready.countDown();
            try { if (binding.registered) context.unbindService(binding); }
            catch (RuntimeException e) { System.err.println("Shared microG unbind failed: " + e); }
        }
        Integer[] streamIds;
        synchronized (providerStreams) { streamIds = providerStreams.keySet().toArray(new Integer[0]); }
        for (Integer id : streamIds) closeProviderStream(id);
        if (accountServer != null) accountServer.close();
    }
}
