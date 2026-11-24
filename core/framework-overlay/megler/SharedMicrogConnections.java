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
    interface DisconnectListener { void disconnected(long channel); }

    private final Context context;
    private final DisconnectListener listener;
    private final HashMap<Long, Connection> connections = new HashMap<>();
    private boolean closed;

    private static final class Connection {
        final long id;
        final ParcelFileDescriptor descriptor;
        final SharedMicrogServiceRouter router;
        final BinderBridge bridge;
        final CountDownLatch finished = new CountDownLatch(1);
        boolean closing;

        Connection(long id, ParcelFileDescriptor descriptor, SharedMicrogServiceRouter router,
                   BinderBridge bridge) {
            this.id = id;
            this.descriptor = descriptor;
            this.router = router;
            this.bridge = bridge;
        }
    }

    SharedMicrogConnections(Context context, DisconnectListener listener) {
        if (context == null || listener == null) throw new IllegalArgumentException("Worker context and listener required");
        this.context = context;
        this.listener = listener;
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
                    @Override public boolean launch(long channelId, Intent intent) {
                        if (intent == null || intent.getComponent() == null ||
                                !"com.google.android.gms".equals(intent.getComponent().getPackageName()) ||
                                Manifest.activity(intent.getComponent().getClassName()) == null ||
                                AppRunner.application() == null || !SharedMicrogWorker.showUi(channelId))
                            return false;
                        try {
                            AppRunner.startActivity(AppRunner.application(),
                                new Intent(intent).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), -1, null);
                            return true;
                        } catch (RuntimeException e) {
                            SharedMicrogWorker.dismissUi(channelId);
                            return false;
                        }
                    }

                    @Override public void dismiss(long channelId) {
                        SharedMicrogWorker.dismissUi(channelId);
                    }
                }, id, packageName, uid, pid);
            router = new SharedMicrogServiceRouter(context, accounts);
            bridge = new BinderBridge(new FileInputStream(descriptor.getFileDescriptor()),
                new FileOutputStream(descriptor.getFileDescriptor()), router.root(), () -> {
                    try { Os.shutdown(descriptor.getFileDescriptor(), SHUT_RDWR); }
                    catch (ErrnoException e) { throw new IOException("socket shutdown failed", e); }
                }, uid, pid);
            Connection connection = new Connection(id, descriptor, router, bridge);
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
            synchronized (this) { connections.remove(connection.id); }
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
    }
}
