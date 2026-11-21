package megler;

import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.concurrent.Executor;

/** Opt-in client-side bind routing over a manager-authorized BinderBridge root. */
public final class SharedMicrogClientRouter implements AutoCloseable {
    private static final String PACKAGE = "com.google.android.gms";
    private static final int BIND = 1;
    private static final int UNBIND = 2;
    private static final int MAX_BINDINGS = 128;
    private final IBinder root;
    private final Executor callbacks;
    private final Object lock = new Object();
    private final IdentityHashMap<ServiceConnection, Binding> bindings = new IdentityHashMap<>();
    private final IBinder.DeathRecipient deathRecipient = this::rootDied;
    private int nextId = 1;
    private boolean closed;

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
        this(root, mainThreadCallbacks());
    }

    SharedMicrogClientRouter(IBinder root, Executor callbacks) {
        if (root == null || callbacks == null) throw new IllegalArgumentException("Binder root and callback executor required");
        this.root = root;
        this.callbacks = callbacks;
        try {
            root.linkToDeath(deathRecipient, 0);
        } catch (RemoteException e) {
            throw new IllegalStateException("Shared microG channel is already closed.", e);
        }
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
        if (intent == null) return false;
        if (intent.getPackage() != null && !PACKAGE.equals(intent.getPackage())) return false;
        ComponentName component = intent.getComponent();
        if (component == null) {
            if (!PACKAGE.equals(intent.getPackage())) return false;
        } else if (!PACKAGE.equals(component.getPackageName())) {
            return false;
        }
        String action = intent.getAction();
        String className = component == null ? null : component.getClassName();
        return (action != null || className != null) &&
            (action == null || (!action.isEmpty() && action.length() <= 4096)) &&
            (className == null || (!className.isEmpty() && className.length() <= 4096)) &&
            intent.getData() == null && intent.getType() == null &&
            intent.getCategories() == null && intent.getExtras() == null &&
            intent.getSelector() == null && intent.getClipData() == null &&
            intent.getSourceBounds() == null && intent.getIdentifier() == null &&
            intent.getFlags() == 0;
    }

    static boolean targetsSharedMicrog(Intent intent) {
        return intent != null && (PACKAGE.equals(intent.getPackage()) ||
            (intent.getComponent() != null && PACKAGE.equals(intent.getComponent().getPackageName())));
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
            request.writeString(intent.getAction());
            ComponentName component = intent.getComponent();
            request.writeString(component == null ? null : component.getClassName());
            if (!root.transact(BIND, request, reply, 0))
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
        }
        RuntimeException failure = null;
        if (root.isBinderAlive()) {
            for (Binding binding : active) {
                try { unbindRemote(binding.id); }
                catch (RuntimeException e) {
                    if (failure == null) failure = e;
                    else failure.addSuppressed(e);
                }
            }
        }
        root.unlinkToDeath(deathRecipient, 0);
        if (failure != null) throw failure;
    }
}
