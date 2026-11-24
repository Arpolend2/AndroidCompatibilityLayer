package megler;

import android.content.Context;
import android.content.Intent;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.concurrent.CountDownLatch;

/** Owns the guest resources for manager-authorized channels in one worker. */
final class SharedMicrogConnections implements AutoCloseable {
    // OsConstants are not initialized by the emulated Android runtime.
    private static final int SHUT_RDWR = 2;
    private static volatile SharedMicrogConnections current;
    interface DisconnectListener { void disconnected(long channel); }

    private final Context context;
    private final DisconnectListener listener;
    private final HashMap<Long, Connection> connections = new HashMap<>();
    private boolean closed;

    private static final class Connection {
        final long id;
        final String packageName;
        final ParcelFileDescriptor descriptor;
        final SharedMicrogServiceRouter router;
        final BinderBridge bridge;
        final CountDownLatch finished = new CountDownLatch(1);
        boolean closing;

        Connection(long id, String packageName, ParcelFileDescriptor descriptor, SharedMicrogServiceRouter router,
                   BinderBridge bridge) {
            this.id = id;
            this.packageName = packageName;
            this.descriptor = descriptor;
            this.router = router;
            this.bridge = bridge;
        }
    }

    SharedMicrogConnections(Context context, DisconnectListener listener) {
        if (context == null || listener == null) throw new IllegalArgumentException("Worker context and listener required");
        this.context = context;
        this.listener = listener;
        synchronized (SharedMicrogConnections.class) { current = this; }
    }

    static boolean hasClientTarget(Intent intent) {
        SharedMicrogConnections owner = current;
        if (owner == null || intent == null) return false;
        String target = intent.getPackage();
        if (intent.getComponent() != null) {
            String componentPackage = intent.getComponent().getPackageName();
            if (target != null && !target.equals(componentPackage)) return false;
            target = componentPackage;
        }
        synchronized (owner) {
            if (target == null) return !owner.connections.isEmpty();
            for (Connection connection : owner.connections.values())
                if (target.equals(connection.packageName)) return true;
        }
        return false;
    }

    static void sendBroadcastToClients(Intent intent, String permission, String also, boolean holdsAlso,
                                       int uid, String packageName) {
        SharedMicrogConnections owner = current;
        if (owner == null) return;
        if (intent == null) throw new IllegalArgumentException("Shared microG broadcast intent is required");
        Connection[] pending;
        synchronized (owner) { pending = owner.connections.values().toArray(new Connection[0]); }
        String target = intent.getPackage();
        if (intent.getComponent() != null) {
            String componentPackage = intent.getComponent().getPackageName();
            if (target != null && !target.equals(componentPackage)) return;
            target = componentPackage;
        }
        boolean hasTarget = false;
        for (Connection connection : pending)
            if (target == null || target.equals(connection.packageName)) { hasTarget = true; break; }
        if (!hasTarget) return;
        if (!SharedMicrogClientRouter.canTransfer(intent))
            throw new IllegalArgumentException("Unsupported shared microG worker broadcast");
        if ((uid == -1 && packageName != null) ||
                (uid != -1 && (uid <= 0 || !"com.google.android.gms".equals(packageName))))
            throw new IllegalArgumentException("Invalid shared microG broadcast sender");
        for (Connection connection : pending) {
            if (target == null || target.equals(connection.packageName))
                connection.router.broadcastToClient(intent, permission, also, holdsAlso, uid, packageName);
        }
    }

    static void sendOrderedBroadcastToClient(Intent intent, int code, String data, android.os.Bundle extras,
                                             SharedMicrogServiceRouter.OrderedCompletion completion) {
        SharedMicrogConnections owner = current;
        if (owner == null || intent == null || intent.getComponent() == null)
            throw new IllegalStateException("A single explicit shared microG client target is required");
        String target = intent.getComponent().getPackageName();
        if (intent.getPackage() != null && !intent.getPackage().equals(target))
            throw new IllegalArgumentException("Conflicting shared microG ordered broadcast target");
        Connection selected = null;
        synchronized (owner) {
            for (Connection connection : owner.connections.values()) {
                if (!connection.closing && target.equals(connection.packageName)) {
                    if (selected != null)
                        throw new UnsupportedOperationException("Ordered broadcasts to multiple client channels are unsupported");
                    selected = connection;
                }
            }
        }
        if (selected == null) throw new IllegalStateException("Shared microG client channel is unavailable");
        selected.router.broadcastOrderedToClient(intent, code, data, extras, completion);
    }

    synchronized void open(long id, String packageName, int uid, int pid, ParcelFileDescriptor descriptor)
            throws IOException {
        if (descriptor == null) throw new IllegalArgumentException("Channel descriptor required");
        if (closed || connections.size() >= 128 || connections.containsKey(id) || pid <= 0) {
            descriptor.close();
            throw new IllegalArgumentException("Channel is unavailable");
        }
        SharedMicrogServiceRouter router = null;
        BinderBridge bridge = null;
        boolean registered = false;
        try {
            SharedMicrogIdentity.add(id, packageName, uid);
            registered = true;
            SharedMicrogAccounts.Server accounts = new SharedMicrogAccounts.Server(
                new SharedMicrogAccountEngine(context), new SharedMicrogAccounts.Foreground() {
                    @Override public long launch(long channelId, Intent intent) {
                        if (intent == null || intent.getComponent() == null ||
                                !"com.google.android.gms".equals(intent.getComponent().getPackageName()) ||
                                Manifest.activity(intent.getComponent().getClassName()) == null ||
                                AppRunner.application() == null)
                            return 0;
                        long generation = SharedMicrogWorker.showUi(channelId);
                        if (generation == 0) return 0;
                        try {
                            AppRunner.startSharedMicrogActivity(
                                new Intent(intent).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), channelId, generation);
                            return generation;
                        } catch (RuntimeException e) {
                            SharedMicrogWorker.dismissUi(channelId, generation);
                            return 0;
                        }
                    }

                    @Override public void dismiss(long channelId, long generation) {
                        SharedMicrogWorker.dismissUi(channelId, generation);
                    }
                }, id, packageName, uid, pid);
            router = new SharedMicrogServiceRouter(context, accounts);
            bridge = new BinderBridge(new FileInputStream(descriptor.getFileDescriptor()),
                new FileOutputStream(descriptor.getFileDescriptor()), router.root(), () -> {
                    try { Os.shutdown(descriptor.getFileDescriptor(), SHUT_RDWR); }
                    catch (ErrnoException e) { throw new IOException("socket shutdown failed", e); }
                }, uid, pid, SharedMicrogFdTransport.available() ?
                    BinderBridge.descriptorTransport(descriptor.getFd()) : null);
            Connection connection = new Connection(id, packageName, descriptor, router, bridge);
            connections.put(id, connection);
            Thread monitor = new Thread(() -> monitor(connection), "megler-microg-channel");
            monitor.setDaemon(true);
            monitor.start();
        } catch (RuntimeException e) {
            try {
                connections.remove(id);
                if (bridge != null) bridge.close();
                if (router != null) router.close();
            } finally {
                if (registered) SharedMicrogIdentity.remove(id);
                try { descriptor.close(); }
                catch (IOException closeError) { e.addSuppressed(closeError); }
            }
            throw e;
        }
    }

    private void monitor(Connection connection) {
        try {
            connection.bridge.awaitClosed();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            System.err.println("Shared microG channel monitor interrupted: " + e);
        }
        if (finish(connection)) listener.disconnected(connection.id);
    }

    void closeChannel(long id) throws InterruptedException {
        Connection connection;
        synchronized (this) { connection = connections.get(id); }
        if (connection != null && !finish(connection)) connection.finished.await();
    }

    private boolean finish(Connection connection) {
        synchronized (this) {
            if (connection.closing) return false;
            connection.closing = true;
        }
        try {
            connection.bridge.close();
            boolean interrupted = false;
            for (;;) {
                try { connection.bridge.awaitClosed(); break; }
                catch (InterruptedException e) { interrupted = true; }
            }
            if (interrupted) Thread.currentThread().interrupt();
            try { connection.router.close(); }
            catch (RuntimeException e) { System.err.println("Shared microG router close failed: " + e); }
        } finally {
            SharedMicrogIdentity.remove(connection.id);
            try { connection.descriptor.close(); }
            catch (IOException e) { System.err.println("Shared microG descriptor close failed: " + e); }
            boolean empty;
            synchronized (this) {
                connections.remove(connection.id);
                empty = connections.isEmpty();
            }
            if (empty) synchronized (SharedMicrogConnections.class) {
                if (current == this) current = null;
            }
            connection.finished.countDown();
        }
        return true;
    }

    @Override public void close() throws InterruptedException {
        Connection[] pending;
        synchronized (this) {
            if (closed) return;
            closed = true;
            pending = connections.values().toArray(new Connection[0]);
        }
        for (Connection connection : pending)
            if (!finish(connection)) connection.finished.await();
        synchronized (SharedMicrogConnections.class) {
            if (current == this) current = null;
        }
    }
}
