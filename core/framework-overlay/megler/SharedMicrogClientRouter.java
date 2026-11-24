package megler;

import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentValues;
import android.content.Intent;
import android.content.OperationApplicationException;
import android.content.ServiceConnection;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.ContentObserver;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.concurrent.Executor;

/** Opt-in client-side bind routing over a manager-authorized BinderBridge root. */
public final class SharedMicrogClientRouter implements AutoCloseable {
    private static final String PACKAGE = "com.google.android.gms";
    private static final int BIND_LEGACY = SharedMicrogServiceRouter.BIND;
    private static final int UNBIND = SharedMicrogServiceRouter.UNBIND;
    private static final int BIND_INTENT = SharedMicrogServiceRouter.BIND_INTENT;
    private static final int START_SERVICE = SharedMicrogServiceRouter.START_SERVICE;
    private static final int STOP_SERVICE = SharedMicrogServiceRouter.STOP_SERVICE;
    private static final int SEND_BROADCAST = SharedMicrogServiceRouter.SEND_BROADCAST;
    private static final int REGISTER_CLIENT_CALLBACK = SharedMicrogServiceRouter.REGISTER_CLIENT_CALLBACK;
    private static final int UNREGISTER_CLIENT_CALLBACK = SharedMicrogServiceRouter.UNREGISTER_CLIENT_CALLBACK;
    private static final int BROADCAST_FROM_WORKER = SharedMicrogServiceRouter.BROADCAST_FROM_WORKER;
    private static final int PROVIDER_EXISTS = SharedMicrogServiceRouter.PROVIDER_EXISTS;
    private static final int PROVIDER_GET_TYPE = SharedMicrogServiceRouter.PROVIDER_GET_TYPE;
    private static final int PROVIDER_QUERY = SharedMicrogServiceRouter.PROVIDER_QUERY;
    private static final int PROVIDER_CALL = SharedMicrogServiceRouter.PROVIDER_CALL;
    private static final int PROVIDER_INSERT = SharedMicrogServiceRouter.PROVIDER_INSERT;
    private static final int PROVIDER_UPDATE = SharedMicrogServiceRouter.PROVIDER_UPDATE;
    private static final int PROVIDER_DELETE = SharedMicrogServiceRouter.PROVIDER_DELETE;
    private static final int PROVIDER_GET_STREAM_TYPES = SharedMicrogServiceRouter.PROVIDER_GET_STREAM_TYPES;
    private static final int PROVIDER_CANONICALIZE = SharedMicrogServiceRouter.PROVIDER_CANONICALIZE;
    private static final int PROVIDER_UNCANONICALIZE = SharedMicrogServiceRouter.PROVIDER_UNCANONICALIZE;
    private static final int PROVIDER_REFRESH = SharedMicrogServiceRouter.PROVIDER_REFRESH;
    private static final int PROVIDER_OPEN_FILE = SharedMicrogServiceRouter.PROVIDER_OPEN_FILE;
    private static final int PROVIDER_OPEN_ASSET = SharedMicrogServiceRouter.PROVIDER_OPEN_ASSET;
    private static final int PROVIDER_OPEN_TYPED_ASSET = SharedMicrogServiceRouter.PROVIDER_OPEN_TYPED_ASSET;
    private static final int PROVIDER_READ_STREAM = SharedMicrogServiceRouter.PROVIDER_READ_STREAM;
    private static final int PROVIDER_CLOSE_STREAM = SharedMicrogServiceRouter.PROVIDER_CLOSE_STREAM;
    private static final int PROVIDER_APPLY_BATCH = SharedMicrogServiceRouter.PROVIDER_APPLY_BATCH;
    private static final int PROVIDER_REGISTER_OBSERVER = SharedMicrogServiceRouter.PROVIDER_REGISTER_OBSERVER;
    private static final int PROVIDER_UNREGISTER_OBSERVER = SharedMicrogServiceRouter.PROVIDER_UNREGISTER_OBSERVER;
    private static final int PROVIDER_OBSERVER_CHANGED = SharedMicrogServiceRouter.PROVIDER_OBSERVER_CHANGED;
    private static final int PROVIDER_OPEN_WRITE_STREAM = SharedMicrogServiceRouter.PROVIDER_OPEN_WRITE_STREAM;
    private static final int PROVIDER_WRITE_STREAM = SharedMicrogServiceRouter.PROVIDER_WRITE_STREAM;
    private static final int PROVIDER_FINISH_WRITE_STREAM = SharedMicrogServiceRouter.PROVIDER_FINISH_WRITE_STREAM;
    private static final int PROVIDER_NOTIFY_CHANGE = SharedMicrogServiceRouter.PROVIDER_NOTIFY_CHANGE;
    private static final int SEND_ORDERED_BROADCAST = SharedMicrogServiceRouter.SEND_ORDERED_BROADCAST;
    private static final int ORDERED_BROADCAST_RESULT = SharedMicrogServiceRouter.ORDERED_BROADCAST_RESULT;
    private static final int ORDERED_BROADCAST_FROM_WORKER = SharedMicrogServiceRouter.ORDERED_BROADCAST_FROM_WORKER;
    private static final int ORDERED_BROADCAST_CLIENT_RESULT = SharedMicrogServiceRouter.ORDERED_BROADCAST_CLIENT_RESULT;
    private static final int MAX_BINDINGS = 128;
    private static final int PROVIDER_CHUNK_SIZE = 32768;
    interface BroadcastDispatcher {
        void dispatch(Intent intent, String permission, String also, boolean holdsAlso, int uid, String packageName);
    }
    interface OrderedResult {
        void finish(int code, String data, Bundle extras);
    }
    private final IBinder root;
    private final Executor callbacks;
    private final BroadcastDispatcher broadcastDispatcher;
    private final Object lock = new Object();
    private final IdentityHashMap<ServiceConnection, Binding> bindings = new IdentityHashMap<>();
    private final java.util.HashMap<Integer, ObserverRegistration> providerObservers = new java.util.HashMap<>();
    private final java.util.HashMap<Integer, OrderedResult> orderedBroadcasts = new java.util.HashMap<>();
    private final IBinder.DeathRecipient deathRecipient = this::rootDied;
    private final IBinder workerBroadcastReceiver = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (flags != IBinder.FLAG_ONEWAY || reply != null)
                return false;
            if (code == ORDERED_BROADCAST_FROM_WORKER) {
                int id = data.readInt();
                Intent intent = data.readParcelable(Intent.class.getClassLoader());
                int resultCode = data.readInt();
                String resultData = data.readString();
                Bundle extras = data.readBundle();
                if (id <= 0 || !canTransfer(intent) || intent.getComponent() == null ||
                        (resultData != null && resultData.length() > 8192) ||
                        (extras != null && extras.size() > 128) || data.dataAvail() != 0)
                    throw new RemoteException("Invalid worker ordered broadcast");
                System.err.println("Shared microG client ordered broadcast received");
                try {
                    Broadcasts.sendOrderedFromSharedWorker(intent, resultCode, resultData, extras,
                        (c, d, e) -> sendClientOrderedResult(id, 0, c, d, e));
                } catch (RuntimeException e) {
                    System.err.println("Shared microG client ordered broadcast dispatch failed: " + e);
                    sendClientOrderedResult(id, 1, 0, null, null);
                }
                return true;
            }
            if (code == ORDERED_BROADCAST_RESULT) {
                int id = data.readInt();
                int status = data.readInt();
                int resultCode = data.readInt();
                String resultData = data.readString();
                Bundle extras = data.readBundle();
                if (id <= 0 || (status != 0 && status != 1) || data.dataAvail() != 0 ||
                        (resultData != null && resultData.length() > 8192) || (extras != null && extras.size() > 128))
                    throw new RemoteException("Invalid shared microG ordered broadcast result");
                OrderedResult pending;
                synchronized (lock) { pending = orderedBroadcasts.remove(id); }
                if (pending == null) return true;
                if (status != 0) System.err.println("Shared microG ordered broadcast result could not be transferred.");
                else pending.finish(resultCode, resultData, extras);
                return true;
            }
            if (code == PROVIDER_OBSERVER_CHANGED) {
                int id = data.readInt();
                String text = data.readString();
                if (id <= 0 || text == null || text.length() > 8192 || data.dataAvail() != 0)
                    throw new RemoteException("Invalid shared microG observer callback");
                Uri changed = Uri.parse(text);
                ObserverRegistration registration;
                synchronized (lock) { registration = providerObservers.get(id); }
                if (registration != null && SharedMicrogServiceRouter.matchesObserverUri(
                        registration.uri, changed, registration.descendants))
                    registration.observer.dispatchChange(false, changed);
                return true;
            }
            if (code != BROADCAST_FROM_WORKER) return false;
            Intent intent = data.readParcelable(Intent.class.getClassLoader());
            String permission = data.readString();
            String also = data.readString();
            int rawHoldsAlso = data.readInt();
            boolean holdsAlso = rawHoldsAlso != 0;
            int uid = data.readInt();
            String packageName = data.readString();
            if (data.dataAvail() != 0 || !canTransfer(intent) ||
                    permission != null && permission.length() > 4096 ||
                    also != null && also.length() > 4096 ||
                    rawHoldsAlso != 0 && rawHoldsAlso != 1 ||
                    (uid == -1 && packageName != null) ||
                    (uid != -1 && (uid <= 0 || !"com.google.android.gms".equals(packageName))))
                throw new RemoteException("Invalid shared microG worker broadcast");
            broadcastDispatcher.dispatch(intent, permission, also, holdsAlso, uid, packageName);
            return true;
        }
    };
    private int nextId = 1;
    private int nextObserverId = 1;
    private int nextOrderedId = 1;
    private boolean closed;

    private static final class ObserverRegistration {
        final Uri uri;
        final boolean descendants;
        final ContentObserver observer;
        ObserverRegistration(Uri uri, boolean descendants, ContentObserver observer) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
        }
    }

    private static final class Binding {
        final int id;
        boolean pending = true;
        boolean cancelled;
        ComponentName component;
        IBinder service;

        Binding(int id) { this.id = id; }
    }

    private static final class Disconnection {
        final ServiceConnection connection;
        final Binding binding;
        Disconnection(ServiceConnection connection, Binding binding) {
            this.connection = connection;
            this.binding = binding;
        }
    }

    public SharedMicrogClientRouter(IBinder root) {
        this(root, mainThreadCallbacks(), Broadcasts::sendFromSharedWorker);
    }

    SharedMicrogClientRouter(IBinder root, Executor callbacks) {
        this(root, callbacks, Broadcasts::sendFromSharedWorker);
    }

    SharedMicrogClientRouter(IBinder root, BroadcastDispatcher broadcastDispatcher) {
        this(root, mainThreadCallbacks(), broadcastDispatcher);
    }

    SharedMicrogClientRouter(IBinder root, Executor callbacks, BroadcastDispatcher broadcastDispatcher) {
        if (root == null || callbacks == null || broadcastDispatcher == null)
            throw new IllegalArgumentException("Binder root and callback handlers required");
        this.root = root;
        this.callbacks = callbacks;
        this.broadcastDispatcher = broadcastDispatcher;
        try {
            root.linkToDeath(deathRecipient, 0);
        } catch (RemoteException e) {
            throw new IllegalStateException("Shared microG channel is already closed.", e);
        }
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeStrongBinder(workerBroadcastReceiver);
            if (!root.transact(REGISTER_CLIENT_CALLBACK, request, reply, 0))
                throw new RemoteException("Shared microG rejected the worker broadcast callback.");
            reply.readException();
        } catch (RemoteException | RuntimeException e) {
            root.unlinkToDeath(deathRecipient, 0);
            throw new IllegalStateException("Could not register the shared microG worker broadcast callback.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private void sendClientOrderedResult(int id, int status, int code, String data, Bundle extras) {
        Parcel result = Parcel.obtain();
        try {
            if ((data != null && data.length() > 8192) || (extras != null && extras.size() > 128))
                throw new IllegalArgumentException("Ordered broadcast result exceeds transfer limits");
            result.writeInt(id);
            result.writeInt(status);
            result.writeInt(code);
            result.writeString(data);
            result.writeBundle(extras);
            if (!root.transact(ORDERED_BROADCAST_CLIENT_RESULT, result, null, IBinder.FLAG_ONEWAY))
                throw new RemoteException("Shared microG worker rejected the ordered result");
        } catch (RemoteException | RuntimeException e) {
            System.err.println("Shared microG client ordered broadcast result failed: " + e);
            if (status == 0) sendClientOrderedResult(id, 1, 0, null, null);
        } finally { result.recycle(); }
    }

    private static Executor mainThreadCallbacks() {
        Looper main = Looper.getMainLooper();
        if (main == null) throw new IllegalStateException("Main looper is unavailable.");
        Handler handler = new Handler(main);
        return command -> {
            if (!handler.post(command)) throw new IllegalStateException("Main thread callback queue is unavailable.");
        };
    }

    public static boolean canRoute(Intent intent) {
        if (!canTransfer(intent)) return false;
        if (intent.getPackage() != null && !PACKAGE.equals(intent.getPackage())) return false;
        ComponentName component = intent.getComponent();
        if (component == null) {
            if (!PACKAGE.equals(intent.getPackage())) return false;
        } else if (!PACKAGE.equals(component.getPackageName())) {
            return false;
        }
        return true;
    }

    static boolean canTransfer(Intent intent) {
        if (intent == null) return false;
        String packageName = intent.getPackage();
        if (packageName != null && (packageName.isEmpty() || packageName.length() > 4096)) return false;
        ComponentName component = intent.getComponent();
        if (component != null && (component.getPackageName() == null ||
                component.getPackageName().isEmpty() || component.getPackageName().length() > 4096)) return false;
        String action = intent.getAction();
        String className = component == null ? null : component.getClassName();
        String data = intent.getDataString();
        String type = intent.getType();
        java.util.Set<String> categories = intent.getCategories();
        if (categories != null) {
            if (categories.size() > 64) return false;
            for (String category : categories)
                if (category == null || category.isEmpty() || category.length() > 4096) return false;
        }
        android.os.Bundle extras = intent.getExtras();
        return (action != null || className != null || data != null || type != null ||
                categories != null && !categories.isEmpty()) &&
            (action == null || (!action.isEmpty() && action.length() <= 4096)) &&
            (className == null || (!className.isEmpty() && className.length() <= 4096)) &&
            (data == null || data.length() <= 8192) &&
            (type == null || (!type.isEmpty() && type.length() <= 4096)) &&
            (extras == null || extras.size() <= 1024) &&
            intent.getSelector() == null && intent.getClipData() == null &&
            intent.getSourceBounds() == null && intent.getIdentifier() == null &&
            intent.getFlags() == 0;
    }

    private static boolean canUseLegacyBind(Intent intent) {
        return intent.getData() == null && intent.getType() == null &&
            intent.getCategories() == null && intent.getExtras() == null;
    }

    static boolean targetsSharedMicrog(Intent intent) {
        return intent != null && (PACKAGE.equals(intent.getPackage()) ||
            (intent.getComponent() != null && PACKAGE.equals(intent.getComponent().getPackageName())));
    }

    static boolean canRouteOrdered(Intent intent) {
        return canRoute(intent) && (intent.getComponent() != null ||
            intent.getData() == null && intent.getType() == null &&
            (intent.getCategories() == null || intent.getCategories().isEmpty()));
    }

    public boolean bind(Intent intent, ServiceConnection connection) {
        if (!canRoute(intent)) return false;
        if (connection == null) throw new IllegalArgumentException("Service connection is required.");
        Binding binding;
        synchronized (lock) {
            if (closed) throw new IllegalStateException("Shared microG client router is closed.");
            if (bindings.containsKey(connection)) throw new IllegalArgumentException("Service connection is already bound.");
            if (bindings.size() >= MAX_BINDINGS) throw new IllegalStateException("Shared microG binding limit reached.");
            binding = new Binding(allocateId());
            bindings.put(connection, binding);
        }

        String className = null;
        IBinder service = null;
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInt(binding.id);
            int transaction;
            if (canUseLegacyBind(intent)) {
                transaction = BIND_LEGACY;
                request.writeString(intent.getAction());
                ComponentName component = intent.getComponent();
                request.writeString(component == null ? null : component.getClassName());
            } else {
                transaction = BIND_INTENT;
                request.writeParcelable(intent, 0);
            }
            if (!root.transact(transaction, request, reply, 0))
                throw new RemoteException("Shared microG rejected the bind transaction.");
            reply.readException();
            className = reply.readString();
            service = reply.readStrongBinder();
        } catch (RemoteException | RuntimeException e) {
            remove(binding, connection);
            throw new IllegalStateException("Shared microG service bind failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }

        if (className == null || className.isEmpty() || className.length() > 4096) {
            remove(binding, connection);
            unbindRemote(binding.id);
            if (className != null || service != null) {
                throw new IllegalStateException("Shared microG returned an invalid service component.");
            }
            return false;
        }
        binding.component = new ComponentName(PACKAGE, className);
        binding.service = service;
        boolean cancelled;
        synchronized (lock) {
            cancelled = closed || binding.cancelled || bindings.get(connection) != binding;
            if (!cancelled) binding.pending = false;
            else bindings.remove(connection);
        }
        if (cancelled) {
            if (root.isBinderAlive()) unbindRemote(binding.id);
            return true;
        }

        dispatch(connection, binding, service == null);
        return true;
    }

    public ComponentName startService(Intent intent) {
        if (!canRoute(intent)) return null;
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeParcelable(intent, 0);
            if (!root.transact(START_SERVICE, request, reply, 0))
                throw new RemoteException("Shared microG rejected the start-service transaction.");
            reply.readException();
            String className = reply.readString();
            if (className == null) return null;
            if (className.isEmpty() || className.length() > 4096)
                throw new IllegalStateException("Shared microG returned an invalid service component.");
            return new ComponentName(PACKAGE, className);
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG service start failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    public boolean stopService(Intent intent) {
        if (!canRoute(intent)) return false;
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeParcelable(intent, 0);
            if (!root.transact(STOP_SERVICE, request, reply, 0))
                throw new RemoteException("Shared microG rejected the stop-service transaction.");
            reply.readException();
            return reply.readInt() != 0;
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG service stop failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    public void sendBroadcast(Intent intent) {
        if (!canRoute(intent)) throw new IllegalArgumentException("Unsupported shared microG broadcast intent.");
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeParcelable(intent, 0);
            if (!root.transact(SEND_BROADCAST, request, reply, 0))
                throw new RemoteException("Shared microG rejected the broadcast transaction.");
            reply.readException();
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG broadcast failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    void sendOrderedBroadcast(Intent intent, int code, String data, Bundle extras, OrderedResult result) {
        if (!canRouteOrdered(intent))
            throw new UnsupportedOperationException("Unsupported shared microG ordered broadcast intent.");
        if ((data != null && data.length() > 8192) || (extras != null && extras.size() > 128) || result == null)
            throw new IllegalArgumentException("Invalid shared microG ordered broadcast result.");
        final int id;
        synchronized (lock) {
            if (closed || orderedBroadcasts.size() >= 128)
                throw new IllegalStateException("Shared microG ordered broadcast registry unavailable.");
            int candidate;
            do {
                candidate = nextOrderedId++;
                if (nextOrderedId <= 0) nextOrderedId = 1;
            } while (orderedBroadcasts.containsKey(candidate));
            id = candidate;
            orderedBroadcasts.put(id, result);
        }
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInt(id);
            request.writeParcelable(intent, 0);
            request.writeInt(code);
            request.writeString(data);
            request.writeBundle(extras);
            if (!root.transact(SEND_ORDERED_BROADCAST, request, reply, 0))
                throw new RemoteException("Shared microG rejected the ordered broadcast.");
            reply.readException();
            if (reply.dataAvail() != 0) throw new RemoteException("Unexpected ordered broadcast response.");
        } catch (RemoteException | RuntimeException e) {
            synchronized (lock) { orderedBroadcasts.remove(id); }
            throw new IllegalStateException("Shared microG ordered broadcast failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    ContentProvider provider(String authority) {
        validateProviderAuthority(authority);
        boolean available = providerTransact(PROVIDER_EXISTS, data -> data.writeString(authority),
            reply -> reply.readInt() != 0);
        return available ? new SharedProvider(this, authority) : null;
    }

    public void registerProviderObserver(Uri uri, boolean descendants, ContentObserver observer) {
        if (uri == null || observer == null) throw new IllegalArgumentException("Observer and URI are required.");
        String authority = uri.getAuthority();
        validateProviderUri(authority, uri);
        synchronized (lock) {
            if (closed || providerObservers.size() >= 128)
                throw new IllegalStateException("Shared microG observer registration unavailable.");
            int id;
            do {
                id = nextObserverId++;
                if (nextObserverId <= 0) nextObserverId = 1;
            } while (providerObservers.containsKey(id));
            final int registrationId = id;
            providerObservers.put(id, new ObserverRegistration(uri, descendants, observer));
            try {
                providerTransact(PROVIDER_REGISTER_OBSERVER, data -> {
                    data.writeString(authority);
                    data.writeString(uri.toString());
                    data.writeInt(descendants ? 1 : 0);
                    data.writeInt(registrationId);
                }, reply -> null);
            } catch (RuntimeException e) {
                providerObservers.remove(id);
                throw e;
            }
        }
    }

    public void unregisterProviderObserver(ContentObserver observer) {
        synchronized (lock) {
            ArrayList<Integer> ids = new ArrayList<>();
            for (java.util.Map.Entry<Integer, ObserverRegistration> entry : providerObservers.entrySet())
                if (entry.getValue().observer == observer) ids.add(entry.getKey());
            for (Integer id : ids) {
                providerTransact(PROVIDER_UNREGISTER_OBSERVER, data -> data.writeInt(id), reply -> null);
                providerObservers.remove(id);
            }
        }
    }

    public void notifyProviderChange(Uri uri, ContentObserver observer) {
        if (uri == null) throw new IllegalArgumentException("Provider URI is required.");
        String authority = uri.getAuthority();
        validateProviderUri(authority, uri);
        synchronized (lock) {
            if (closed) throw new IllegalStateException("Shared microG client channel is closed.");
            ArrayList<Integer> excluded = new ArrayList<>();
            if (observer != null) {
                for (java.util.Map.Entry<Integer, ObserverRegistration> entry : providerObservers.entrySet())
                    if (entry.getValue().observer == observer) excluded.add(entry.getKey());
            }
            providerTransact(PROVIDER_NOTIFY_CHANGE, data -> {
                data.writeString(authority);
                data.writeString(uri.toString());
                data.writeInt(excluded.size());
                for (Integer id : excluded) data.writeInt(id);
            }, reply -> null);
        }
    }

    private interface ProviderWriter { void write(Parcel data); }
    private interface ProviderReader<T> { T read(Parcel reply); }

    private <T> T providerTransact(int code, ProviderWriter writer, ProviderReader<T> reader) {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            writer.write(request);
            if (!root.transact(code, request, reply, 0))
                throw new RemoteException("Shared microG rejected the provider transaction.");
            reply.readException();
            T value = reader.read(reply);
            if (reply.dataAvail() != 0) throw new RemoteException("Unexpected shared microG provider response data.");
            return value;
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG provider transaction failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private static void validateProviderAuthority(String authority) {
        if (authority == null || !authority.startsWith("com.google.") ||
                authority.length() > 4096 || authority.indexOf(';') >= 0)
            throw new IllegalArgumentException("Invalid shared microG provider authority.");
    }

    private static void validateProviderUri(String authority, Uri uri) {
        validateProviderAuthority(authority);
        if (uri == null || !"content".equals(uri.getScheme()) || !authority.equals(uri.getAuthority()) ||
                uri.toString().length() > 8192)
            throw new IllegalArgumentException("Invalid shared microG provider URI.");
    }

    private static void writeProviderStrings(Parcel data, String[] values, int maxItems, int maxLength) {
        if (values != null) {
            if (values.length > maxItems) throw new IllegalArgumentException("Provider string array exceeds limit.");
            for (String value : values)
                if (value != null && value.length() > maxLength)
                    throw new IllegalArgumentException("Provider string array item exceeds limit.");
        }
        data.writeStringArray(values);
    }

    private Cursor queryProvider(String authority, Uri uri, String[] projection, String selection,
                                 String[] selectionArgs, String sortOrder, Bundle queryArgs) {
        validateProviderUri(authority, uri);
        return providerTransact(PROVIDER_QUERY, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
            data.writeInt(queryArgs == null ? 0 : 1);
            writeProviderStrings(data, projection, 128, 4096);
            if (queryArgs != null) data.writeBundle(queryArgs);
            else {
                if (selection != null && selection.length() > 8192 ||
                        sortOrder != null && sortOrder.length() > 8192)
                    throw new IllegalArgumentException("Provider query string exceeds limit.");
                data.writeString(selection);
                writeProviderStrings(data, selectionArgs, 128, 4096);
                data.writeString(sortOrder);
            }
        }, this::readProviderCursor);
    }

    private Cursor readProviderCursor(Parcel reply) {
        int columnCount = reply.readInt();
        if (columnCount == -1) return null;
        if (columnCount < 0 || columnCount > 128) throw new IllegalStateException("Invalid provider cursor columns.");
        String[] columns = new String[columnCount];
        for (int i = 0; i < columns.length; i++) {
            columns[i] = reply.readString();
            if (columns[i] == null || columns[i].length() > 4096)
                throw new IllegalStateException("Invalid provider cursor column.");
        }
        int rows = reply.readInt();
        if (rows < 0 || rows > 1024) throw new IllegalStateException("Invalid provider cursor row count.");
        MatrixCursor cursor = new MatrixCursor(columns, rows);
        int total = 0;
        for (int row = 0; row < rows; row++) {
            Object[] values = new Object[columnCount];
            for (int column = 0; column < columnCount; column++) {
                switch (reply.readInt()) {
                    case Cursor.FIELD_TYPE_NULL: values[column] = null; break;
                    case Cursor.FIELD_TYPE_INTEGER: values[column] = reply.readLong(); total += 8; break;
                    case Cursor.FIELD_TYPE_FLOAT: values[column] = reply.readDouble(); total += 8; break;
                    case Cursor.FIELD_TYPE_STRING: {
                        String value = reply.readString();
                        if (value != null && value.length() > 65536)
                            throw new IllegalStateException("Provider cursor string exceeds limit.");
                        total += value == null ? 0 : value.length() * 2;
                        values[column] = value;
                        break;
                    }
                    case Cursor.FIELD_TYPE_BLOB: {
                        byte[] value = reply.createByteArray();
                        if (value != null && value.length > 262144)
                            throw new IllegalStateException("Provider cursor blob exceeds limit.");
                        total += value == null ? 0 : value.length;
                        values[column] = value;
                        break;
                    }
                    default: throw new IllegalStateException("Unsupported provider cursor value type.");
                }
                if (total > 524288) throw new IllegalStateException("Provider cursor exceeds transfer limit.");
            }
            cursor.addRow(values);
        }
        Bundle extras = reply.readBundle(Bundle.class.getClassLoader());
        if (extras != null && extras.size() > 128) throw new IllegalStateException("Provider cursor extras exceed limit.");
        cursor.setExtras(extras);
        String notificationUri = reply.readString();
        if (notificationUri != null) cursor.setNotificationUri(android.content.ContentResolver.self(), Uri.parse(notificationUri));
        int wantsAllOnMove = reply.readInt();
        if (wantsAllOnMove != 0) throw new UnsupportedOperationException("Provider cursor all-on-move behavior is unsupported.");
        return cursor;
    }

    private Bundle callProvider(String authority, Uri uri, String method, String arg, Bundle extras) {
        validateProviderUri(authority, uri);
        if (method != null && method.length() > 4096 || arg != null && arg.length() > 4096)
            throw new IllegalArgumentException("Provider call string exceeds limit.");
        return providerTransact(PROVIDER_CALL, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
            data.writeString(method);
            data.writeString(arg);
            data.writeBundle(extras);
        }, reply -> reply.readBundle(Bundle.class.getClassLoader()));
    }

    private String getProviderType(String authority, Uri uri) {
        validateProviderUri(authority, uri);
        return providerTransact(PROVIDER_GET_TYPE, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
        }, Parcel::readString);
    }

    private int updateOrDeleteProvider(int code, String authority, Uri uri, ContentValues values,
                                       String selection, String[] selectionArgs) {
        validateProviderUri(authority, uri);
        return providerTransact(code, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
            if (code == PROVIDER_UPDATE) SharedMicrogServiceRouter.writeContentValues(data, values);
            writeProviderString(data, selection, 8192);
            writeProviderStrings(data, selectionArgs, 128, 4096);
        }, Parcel::readInt);
    }

    private static void writeProviderString(Parcel data, String value, int maxLength) {
        if (value != null && value.length() > maxLength)
            throw new IllegalArgumentException("Provider string exceeds limit.");
        data.writeString(value);
    }

    private String[] providerStreamTypes(String authority, Uri uri, String filter) {
        validateProviderUri(authority, uri);
        return providerTransact(PROVIDER_GET_STREAM_TYPES, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
            writeProviderString(data, filter, 4096);
        }, reply -> {
            String[] values = reply.createStringArray();
            if (values != null && values.length > 128)
                throw new IllegalStateException("Too many provider stream types.");
            return values;
        });
    }

    private Uri canonicalizeProvider(int code, String authority, Uri uri) {
        validateProviderUri(authority, uri);
        return providerTransact(code, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
        }, reply -> {
            String value = reply.readString();
            return value == null ? null : Uri.parse(value);
        });
    }

    private boolean refreshProvider(String authority, Uri uri, Bundle extras) {
        validateProviderUri(authority, uri);
        return providerTransact(PROVIDER_REFRESH, data -> {
            data.writeString(authority);
            data.writeString(uri.toString());
            data.writeBundle(extras);
        }, reply -> reply.readInt() != 0);
    }

    private ContentProviderResult[] applyProviderBatch(String authority,
            ArrayList<ContentProviderOperation> operations) throws OperationApplicationException {
        validateProviderAuthority(authority);
        if (operations == null || operations.size() > 128)
            throw new IllegalArgumentException("Provider batch exceeds limit.");
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeString(authority);
            request.writeInt(operations.size());
            for (ContentProviderOperation operation : operations) {
                if (operation == null) throw new IllegalArgumentException("Null provider batch operation.");
                validateProviderUri(authority, operation.getUri());
                request.writeTypedObject(operation, 0);
            }
            if (!root.transact(PROVIDER_APPLY_BATCH, request, reply, 0))
                throw new RemoteException("Shared microG rejected the provider batch.");
            reply.readException();
            int count = reply.readInt();
            if (count == -1) {
                String message = reply.readString();
                if (reply.dataAvail() != 0) throw new RemoteException("Unexpected provider batch error data.");
                throw new OperationApplicationException(message == null
                    ? "Shared microG provider batch failed." : message);
            }
            if (count != operations.size()) throw new RemoteException("Invalid provider batch result count.");
            ContentProviderResult[] results = new ContentProviderResult[count];
            for (int i = 0; i < count; i++) {
                results[i] = reply.readTypedObject(ContentProviderResult.CREATOR);
                if (results[i] == null) throw new RemoteException("Invalid provider batch result.");
            }
            if (reply.dataAvail() != 0) throw new RemoteException("Unexpected provider batch response data.");
            return results;
        } catch (OperationApplicationException e) {
            throw e;
        } catch (RemoteException | RuntimeException e) {
            throw new OperationApplicationException("Shared microG provider batch failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private static final class ProviderFile {
        final ParcelFileDescriptor descriptor;
        final long length;
        ProviderFile(ParcelFileDescriptor descriptor, long length) {
            this.descriptor = descriptor;
            this.length = length;
        }
    }

    public OutputStream openProviderOutputStream(Uri uri, String mode) throws FileNotFoundException {
        if (uri == null) throw new FileNotFoundException("Provider URI is required.");
        String authority = uri.getAuthority();
        validateProviderUri(authority, uri);
        if (!"w".equals(mode) && !"wt".equals(mode) && !"wa".equals(mode))
            throw new UnsupportedOperationException("Unsupported shared microG provider write mode.");
        final int id;
        try {
            id = providerTransact(PROVIDER_OPEN_WRITE_STREAM, data -> {
                data.writeString(authority);
                data.writeString(uri.toString());
                data.writeString(mode);
            }, reply -> reply.readInt());
            if (id <= 0) throw new IllegalStateException("Invalid shared microG provider stream ID.");
        } catch (RuntimeException e) {
            FileNotFoundException failure = new FileNotFoundException("Shared microG provider write open failed.");
            failure.initCause(e);
            throw failure;
        }
        return new OutputStream() {
            private boolean finished;
            @Override public synchronized void write(int value) throws IOException {
                write(new byte[] { (byte) value }, 0, 1);
            }
            @Override public synchronized void write(byte[] bytes, int offset, int length) throws IOException {
                if (bytes == null) throw new NullPointerException("bytes");
                if (offset < 0 || length < 0 || offset > bytes.length - length)
                    throw new IndexOutOfBoundsException();
                if (finished) throw new IOException("Provider stream is closed.");
                while (length > 0) {
                    int count = Math.min(length, PROVIDER_CHUNK_SIZE);
                    byte[] chunk = java.util.Arrays.copyOfRange(bytes, offset, offset + count);
                    try {
                        providerTransact(PROVIDER_WRITE_STREAM, data -> {
                            data.writeInt(id);
                            data.writeByteArray(chunk);
                        }, reply -> null);
                    } catch (RuntimeException e) {
                        finished = true;
                        closeProviderStream(id);
                        throw new IOException("Shared microG provider stream write failed.", e);
                    }
                    offset += count;
                    length -= count;
                }
            }
            @Override public synchronized void close() throws IOException {
                if (finished) return;
                finished = true;
                try {
                    providerTransact(PROVIDER_FINISH_WRITE_STREAM, data -> data.writeInt(id), reply -> null);
                } catch (RuntimeException e) {
                    throw new IOException("Shared microG provider stream close failed.", e);
                }
            }
        };
    }

    private static final class OpenProviderStream {
        final int id;
        final long length;
        OpenProviderStream(int id, long length) {
            this.id = id;
            this.length = length;
        }
    }

    private ProviderFile openProviderFile(int code, String authority, Uri uri, String mode, String mimeType,
                                          Bundle options) throws FileNotFoundException {
        validateProviderUri(authority, uri);
        if (!"r".equals(mode)) throw new UnsupportedOperationException("Shared microG providers are read-only over IPC.");
        OpenProviderStream opened;
        try {
            opened = providerTransact(code, data -> {
                data.writeString(authority);
                data.writeString(uri.toString());
                if (code == PROVIDER_OPEN_TYPED_ASSET) {
                    writeProviderString(data, mimeType, 4096);
                    data.writeBundle(options);
                } else {
                    data.writeString(mode);
                }
            }, reply -> new OpenProviderStream(reply.readInt(), reply.readLong()));
        } catch (RuntimeException e) {
            FileNotFoundException failure = new FileNotFoundException("Shared microG provider file could not be opened.");
            failure.initCause(e);
            throw failure;
        }
        if (opened.id < 0) return null;
        ParcelFileDescriptor[] pipe;
        try {
            pipe = ParcelFileDescriptor.createPipe();
        } catch (IOException e) {
            closeProviderStream(opened.id);
            FileNotFoundException failure = new FileNotFoundException("Shared microG provider pipe could not be created.");
            failure.initCause(e);
            throw failure;
        }
        try {
            Thread pump = new Thread(() -> pumpProviderStream(opened.id, pipe[1]),
                "megler-shared-microg-provider");
            pump.setDaemon(true);
            pump.start();
        } catch (RuntimeException | Error failure) {
            try { pipe[0].close(); } catch (IOException closeError) { failure.addSuppressed(closeError); }
            try { pipe[1].close(); } catch (IOException closeError) { failure.addSuppressed(closeError); }
            closeProviderStream(opened.id);
            throw failure;
        }
        return new ProviderFile(pipe[0], opened.length);
    }

    private void pumpProviderStream(int id, ParcelFileDescriptor writeEnd) {
        try (OutputStream output = new ParcelFileDescriptor.AutoCloseOutputStream(writeEnd)) {
            for (;;) {
                byte[] chunk = readProviderStream(id);
                if (chunk == null) break;
                if (chunk.length != 0) output.write(chunk);
            }
        } catch (IOException | RuntimeException e) {
            System.err.println("Shared microG provider stream transfer failed: " + e);
        } finally {
            closeProviderStream(id);
        }
    }

    private byte[] readProviderStream(int id) {
        return providerTransact(PROVIDER_READ_STREAM, data -> data.writeInt(id), reply -> {
            byte[] chunk = reply.createByteArray();
            if (chunk != null && chunk.length > PROVIDER_CHUNK_SIZE)
                throw new IllegalStateException("Shared microG provider chunk exceeds limit.");
            return chunk;
        });
    }

    private void closeProviderStream(int id) {
        try {
            providerTransact(PROVIDER_CLOSE_STREAM, data -> data.writeInt(id), reply -> null);
        } catch (RuntimeException e) {
            System.err.println("Shared microG provider stream close failed: " + e);
        }
    }

    private static final class SharedProvider extends ContentProvider {
        private final SharedMicrogClientRouter router;
        private final String authority;

        SharedProvider(SharedMicrogClientRouter router, String authority) {
            this.router = router;
            this.authority = authority;
        }

        @Override public boolean onCreate() { return true; }
        @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
                                      String sortOrder) {
            return router.queryProvider(authority, uri, projection, selection, selectionArgs, sortOrder, null);
        }
        @Override public Cursor query(Uri uri, String[] projection, Bundle queryArgs,
                                      android.os.CancellationSignal cancellationSignal) {
            if (cancellationSignal != null) cancellationSignal.throwIfCanceled();
            return router.queryProvider(authority, uri, projection, null, null, null,
                queryArgs == null ? Bundle.EMPTY : queryArgs);
        }
        @Override public String getType(Uri uri) { return router.getProviderType(authority, uri); }
        @Override public Uri insert(Uri uri, ContentValues values) {
            return router.providerTransact(PROVIDER_INSERT, data -> {
                validateProviderUri(authority, uri);
                data.writeString(authority);
                data.writeString(uri.toString());
                SharedMicrogServiceRouter.writeContentValues(data, values);
            }, reply -> {
                String result = reply.readString();
                return result == null ? null : Uri.parse(result);
            });
        }
        @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
            return router.updateOrDeleteProvider(PROVIDER_UPDATE, authority, uri, values, selection, selectionArgs);
        }
        @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
            return router.updateOrDeleteProvider(PROVIDER_DELETE, authority, uri, null, selection, selectionArgs);
        }
        @Override public Bundle call(String method, String arg, Bundle extras) {
            return router.callProvider(authority, Uri.parse("content://" + authority), method, arg, extras);
        }
        @Override public Bundle call(String authority, String method, String arg, Bundle extras) {
            if (!this.authority.equals(authority)) throw new IllegalArgumentException("Provider authority mismatch.");
            return call(method, arg, extras);
        }
        @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
            ProviderFile file = router.openProviderFile(PROVIDER_OPEN_FILE, authority, uri, mode, null, null);
            return file == null ? null : file.descriptor;
        }
        @Override public AssetFileDescriptor openAssetFile(Uri uri, String mode) throws FileNotFoundException {
            ProviderFile file = router.openProviderFile(PROVIDER_OPEN_ASSET, authority, uri, mode, null, null);
            return file == null ? null : new AssetFileDescriptor(file.descriptor, 0, file.length);
        }
        @Override public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeType, Bundle options)
                throws FileNotFoundException {
            ProviderFile file = router.openProviderFile(PROVIDER_OPEN_TYPED_ASSET, authority, uri, "r", mimeType, options);
            return file == null ? null : new AssetFileDescriptor(file.descriptor, 0, file.length);
        }
        @Override public String[] getStreamTypes(Uri uri, String filter) {
            return router.providerStreamTypes(authority, uri, filter);
        }
        @Override public Uri canonicalize(Uri uri) {
            return router.canonicalizeProvider(PROVIDER_CANONICALIZE, authority, uri);
        }
        @Override public Uri uncanonicalize(Uri uri) {
            return router.canonicalizeProvider(PROVIDER_UNCANONICALIZE, authority, uri);
        }
        @Override public boolean refresh(Uri uri, Bundle extras, android.os.CancellationSignal cancellationSignal) {
            if (cancellationSignal != null) cancellationSignal.throwIfCanceled();
            return router.refreshProvider(authority, uri, extras);
        }
        @Override public ContentProviderResult[] applyBatch(ArrayList<ContentProviderOperation> operations)
                throws OperationApplicationException {
            return router.applyProviderBatch(authority, operations);
        }
    }

    private void unregisterWorkerBroadcastCallback() {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            if (!root.transact(UNREGISTER_CLIENT_CALLBACK, request, reply, 0))
                throw new RemoteException("Shared microG rejected the worker broadcast callback removal.");
            reply.readException();
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Could not remove the shared microG worker broadcast callback.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    public boolean hasBinding(ServiceConnection connection) {
        synchronized (lock) { return bindings.containsKey(connection); }
    }

    public void unbind(ServiceConnection connection) {
        Binding binding;
        synchronized (lock) {
            binding = bindings.get(connection);
            if (binding == null) throw new IllegalArgumentException("Service connection is not bound.");
            if (binding.pending) {
                binding.cancelled = true;
                return;
            }
            bindings.remove(connection);
        }
        unbindRemote(binding.id);
    }

    private int allocateId() {
        for (int attempt = 0; attempt < MAX_BINDINGS + 1; attempt++) {
            int id = nextId++;
            if (nextId <= 0) nextId = 1;
            boolean used = false;
            for (Binding binding : bindings.values()) {
                if (binding.id == id) { used = true; break; }
            }
            if (id > 0 && !used) return id;
        }
        throw new IllegalStateException("Shared microG binding IDs are exhausted.");
    }

    private void remove(Binding binding, ServiceConnection connection) {
        synchronized (lock) {
            if (bindings.get(connection) == binding) bindings.remove(connection);
        }
    }

    private void dispatch(ServiceConnection connection, Binding binding, boolean nullBinding) {
        try {
            callbacks.execute(() -> {
                synchronized (lock) {
                    if (closed || bindings.get(connection) != binding) return;
                }
                if (nullBinding) connection.onNullBinding(binding.component);
                else connection.onServiceConnected(binding.component, binding.service);
            });
        } catch (RuntimeException e) {
            try { unbind(connection); }
            catch (RuntimeException closeError) { e.addSuppressed(closeError); }
            throw new IllegalStateException("Could not dispatch the shared microG bind callback.", e);
        }
    }

    private void unbindRemote(int id) {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInt(id);
            if (!root.transact(UNBIND, request, reply, 0))
                throw new RemoteException("Shared microG rejected the unbind transaction.");
            reply.readException();
        } catch (RemoteException | RuntimeException e) {
            throw new IllegalStateException("Shared microG service unbind failed.", e);
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private void rootDied() {
        List<Disconnection> disconnected = new ArrayList<>();
        synchronized (lock) {
            if (closed) return;
            closed = true;
            for (java.util.Map.Entry<ServiceConnection, Binding> entry : bindings.entrySet()) {
                Binding binding = entry.getValue();
                if (!binding.pending && binding.component != null)
                    disconnected.add(new Disconnection(entry.getKey(), binding));
            }
            bindings.clear();
            providerObservers.clear();
            if (!orderedBroadcasts.isEmpty())
                System.err.println("Shared microG channel closed with ordered broadcasts pending.");
            orderedBroadcasts.clear();
        }
        for (Disconnection disconnection : disconnected) {
            try {
                callbacks.execute(() -> {
                    try { disconnection.connection.onServiceDisconnected(disconnection.binding.component); }
                    catch (RuntimeException e) {
                        System.err.println("Shared microG disconnect callback failed: " + e);
                    }
                });
            } catch (RuntimeException e) {
                System.err.println("Shared microG disconnect callback dispatch failed: " + e);
            }
        }
    }

    @Override public void close() {
        List<Binding> active = new ArrayList<>();
        synchronized (lock) {
            if (closed) return;
            closed = true;
            for (Binding binding : bindings.values()) {
                if (binding.pending) binding.cancelled = true;
                else active.add(binding);
            }
            bindings.clear();
            providerObservers.clear();
            if (!orderedBroadcasts.isEmpty())
                System.err.println("Shared microG channel closed with ordered broadcasts pending.");
            orderedBroadcasts.clear();
        }
        boolean interrupted = Thread.interrupted();
        RuntimeException failure = null;
        try {
            if (!interrupted && root.isBinderAlive()) {
                try { unregisterWorkerBroadcastCallback(); }
                catch (RuntimeException e) {
                    if (Thread.interrupted()) interrupted = true;
                    else failure = e;
                }
                for (Binding binding : active) {
                    if (interrupted || !root.isBinderAlive()) break;
                    try { unbindRemote(binding.id); }
                    catch (RuntimeException e) {
                        if (Thread.interrupted()) interrupted = true;
                        else if (failure == null) failure = e;
                        else failure.addSuppressed(e);
                    }
                }
            }
        } finally {
            root.unlinkToDeath(deathRecipient, 0);
            if (interrupted) Thread.currentThread().interrupt();
        }
        if (failure != null) throw failure;
    }
}
