package megler;

import android.content.Context;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import java.io.IOException;

/** Control messages and caller identities come only from the inherited manager socket. */
final class SharedMicrogWorker {
    private static native boolean enabled();
    private static native String receive();
    private static native boolean send(int type, long channel);
    private static final Object replies = new Object();
    private static boolean active;

    private SharedMicrogWorker() {}

    static void prepareIfEnabled(boolean headless) {
        if (!enabled()) return;
        if (SharedMicrogClientStartup.enabled())
            throw new IllegalStateException("Shared microG worker and client cannot run together");
        if (!headless) throw new IllegalStateException("Shared microG must run headless");
        SharedMicrogIdentity.startWorker();
        active = true;
    }

    static void startIfEnabled(Context context) {
        if (!active) return;
        Thread control = new Thread(() -> run(context), "megler-microg-control");
        control.setDaemon(true);
        control.start();
    }

    private static void run(Context context) {
        SharedMicrogConnections connections = new SharedMicrogConnections(context, channel -> {
            synchronized (replies) {
                if (!send(7, channel)) System.err.println("Shared microG disconnect notification failed");
            }
        });
        try {
            if (!send(1, 0)) throw new IOException("Cannot send worker readiness");
            for (;;) {
                String message = receive();
                if (message == null) break;
                String[] fields = message.split(" ", -1);
                int type = Integer.parseInt(fields[0]);
                long channel = Long.parseUnsignedLong(fields[1], 16);
                if (type == 2) {
                    int uid = Integer.parseInt(fields[2]);
                    int pid = Integer.parseInt(fields[3]);
                    int fd = Integer.parseInt(fields[4]);
                    synchronized (replies) {
                        boolean opened = false;
                        if (fd >= 0) {
                            try {
                                connections.open(channel, fields[5], uid, pid, ParcelFileDescriptor.adoptFd(fd));
                                opened = true;
                            } catch (IllegalArgumentException | IllegalStateException | IOException e) {
                                System.err.println("Shared microG channel rejected: " + e);
                            }
                        }
                        if (!send(opened ? 5 : 6, channel)) throw new IOException("Cannot acknowledge channel");
                    }
                } else if (type == 3) {
                    synchronized (replies) {
                        connections.closeChannel(channel);
                        if (!send(5, channel)) throw new IOException("Cannot acknowledge channel close");
                    }
                } else if (type == 4) {
                    connections.close();
                    if (!send(5, 0)) throw new IOException("Cannot acknowledge worker stop");
                    break;
                } else {
                    throw new IOException("Unexpected worker command");
                }
            }
        } catch (IOException | InterruptedException | RuntimeException e) {
            if (e instanceof InterruptedException) Thread.currentThread().interrupt();
            System.err.println("Shared microG control stopped: " + e);
        } finally {
            try { connections.close(); }
            catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                System.err.println("Shared microG cleanup interrupted: " + e);
            }
            Looper.getMainLooper().quit();
        }
    }
}
