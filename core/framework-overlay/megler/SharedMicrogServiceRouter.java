package megler;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.HashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Per-connection service bindings; the owner closes it when the transport dies. */
public final class SharedMicrogServiceRouter implements AutoCloseable {
    public static final int BIND = 1;
    public static final int UNBIND = 2;
    public static final int ACCOUNTS = 3;
    private static final String PACKAGE = "com.google.android.gms";
    private static final int MAX_BINDINGS = 128;
    private final Context context;
    private final Resolver resolver;
    private final SharedMicrogAccounts.Server accountServer;
    private final HashMap<Integer, Binding> bindings = new HashMap<>();
    private boolean closed;

    interface Resolver { ServiceInfo resolve(Intent intent); }

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
        if (context == null) throw new IllegalArgumentException("Context is required");
        if (resolver == null) throw new IllegalArgumentException("Service resolver is required");
        this.context = context;
        this.resolver = resolver;
        this.accountServer = accountServer;
    }

    private final Binder root = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (flags != 0 || reply == null) throw new RemoteException("Synchronous request required");
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
                Binding binding = bind(id, action, component);
                reply.writeNoException();
                reply.writeString(binding.name == null ? null : binding.name.getClassName());
                reply.writeStrongBinder(binding.binder);
                return true;
            }
            if (code == UNBIND) {
                unbind(data.readInt());
                reply.writeNoException();
                return true;
            }
            return false;
        }
    };

    public IBinder root() { return root; }

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
            binding.registered = context.bindService(intent, binding, Context.BIND_AUTO_CREATE);
            bindings.put(id, binding);
            if (!binding.registered) {
                System.err.println("Shared microG service binding refused: " + intent);
                return binding;
            }
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
        synchronized (bindings) {
            if (closed) return;
            closed = true;
            pending = bindings.values().toArray(new Binding[0]);
            bindings.clear();
        }
        for (Binding binding : pending) {
            binding.ready.countDown();
            try { if (binding.registered) context.unbindService(binding); }
            catch (RuntimeException e) { System.err.println("Shared microG unbind failed: " + e); }
        }
        if (accountServer != null) accountServer.close();
    }
}
