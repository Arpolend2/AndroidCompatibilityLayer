package megler;

import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

/** Bootstrap comes only from the host's verified manager reply, never guest arguments or environment. */
final class SharedMicrogClientStartup {
    static native boolean enabled();
    private static native String takeBootstrap();
    private static ParcelFileDescriptor descriptor;
    private static SharedMicrogClientSession session;
    private static volatile boolean required;

    private SharedMicrogClientStartup() {}

    static boolean required() { return required; }

    static void prepareIfEnabled() throws IOException {
        if (!enabled()) return;
        required = true;
        String[] fields = takeBootstrap().split(" ", -1);
        if (fields.length != 4) throw new IllegalStateException("Invalid shared microG bootstrap.");
        int uid = Integer.parseInt(fields[0]);
        int pid = Integer.parseInt(fields[1]);
        int fd = Integer.parseInt(fields[2]);
        if (fd < 0) throw new IllegalStateException("Shared microG descriptor is unavailable.");
        descriptor = ParcelFileDescriptor.adoptFd(fd);
        try {
            if (pid < 50000) throw new IllegalStateException("Invalid shared microG process identity.");
            SharedMicrogIdentity.startClient(uid, fields[3]);
        } catch (RuntimeException | Error failure) {
            try { descriptor.close(); }
            catch (IOException closeError) { failure.addSuppressed(closeError); }
            descriptor = null;
            throw failure;
        }
    }

    static void startIfEnabled() throws IOException {
        if (!enabled()) return;
        if (descriptor == null || session != null)
            throw new IllegalStateException("Shared microG client startup is unavailable.");
        final ParcelFileDescriptor stream = descriptor;
        session = SharedMicrogClientSession.open(new FileInputStream(stream.getFileDescriptor()),
            new FileOutputStream(stream.getFileDescriptor()), () -> {
                // Linux SHUT_RDWR; guest OsConstants initialization is not implemented.
                try { Os.shutdown(stream.getFileDescriptor(), 2); }
                catch (ErrnoException e) { throw new IOException("Shared microG socket shutdown failed", e); }
            }, null, Broadcasts::sendFromSharedWorker,
            SharedMicrogFdTransport.available() ? BinderBridge.descriptorTransport(stream.getFd()) : null);
    }

    static void close() throws IOException {
        try {
            if (session != null) session.close();
        } catch (RuntimeException | Error failure) {
            try { closeDescriptor(); }
            catch (IOException closeError) { failure.addSuppressed(closeError); }
            throw failure;
        } finally {
            session = null;
        }
        closeDescriptor();
    }

    private static void closeDescriptor() throws IOException {
        if (descriptor == null) return;
        ParcelFileDescriptor stream = descriptor;
        descriptor = null;
        stream.close();
    }
}
