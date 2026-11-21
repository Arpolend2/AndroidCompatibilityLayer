package megler;

import java.io.Closeable;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.Executor;

/**
 * Explicit opt-in ownership of an already authorized client transport.
 * The host must retain its dedicated D-Bus connection until this session closes.
 */
public final class SharedMicrogClientSession implements AutoCloseable {
    private final BinderBridge bridge;
    private final SharedMicrogClientRouter router;
    private boolean closed;

    private SharedMicrogClientSession(BinderBridge bridge, SharedMicrogClientRouter router) {
        this.bridge = bridge;
        this.router = router;
    }

    /**
     * Call after main looper setup and before Application/provider initialization.
     * Takes transport ownership even on failure; shutdown must unblock I/O before stream close.
     */
    public static SharedMicrogClientSession open(InputStream input, OutputStream output,
                                                 Closeable shutdownTransport) {
        return open(input, output, shutdownTransport, null);
    }

    static SharedMicrogClientSession open(InputStream input, OutputStream output,
                                          Closeable shutdownTransport, Executor callbacks) {
        BinderBridge bridge = null;
        SharedMicrogClientRouter router = null;
        try {
            if (shutdownTransport == null)
                throw new IllegalArgumentException("A transport shutdown operation is required.");
            bridge = new BinderBridge(input, output, null, shutdownTransport);
            router = callbacks == null ? new SharedMicrogClientRouter(bridge.remoteRoot()) :
                new SharedMicrogClientRouter(bridge.remoteRoot(), callbacks);
            SharedMicrogClientSession session = new SharedMicrogClientSession(bridge, router);
            Services.installSharedMicrogClientRouter(router);
            return session;
        } catch (RuntimeException | Error failure) {
            if (router != null) {
                try { router.close(); }
                catch (RuntimeException closeError) { failure.addSuppressed(closeError); }
            }
            if (bridge != null) closeBridge(bridge);
            else {
                closeAfterFailure(shutdownTransport, failure);
                closeAfterFailure(output, failure);
                closeAfterFailure(input, failure);
            }
            throw failure;
        }
    }

    private static void closeAfterFailure(Closeable resource, Throwable failure) {
        if (resource == null) return;
        try { resource.close(); }
        catch (Exception closeError) { failure.addSuppressed(closeError); }
    }

    private static void closeBridge(BinderBridge bridge) {
        bridge.close();
        boolean interrupted = false;
        for (;;) {
            try { bridge.awaitClosed(); break; }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    /** An unexpected bridge death leaves its closed router installed until explicit teardown. */
    @Override public void close() {
        synchronized (this) {
            if (closed) return;
            closed = true;
        }
        Services.clearSharedMicrogClientRouter(router);
        try { router.close(); }
        finally { closeBridge(bridge); }
    }
}
