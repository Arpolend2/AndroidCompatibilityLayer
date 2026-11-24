package megler;

import android.content.Context;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.app.Activity;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

/** Control messages and caller identities come only from the inherited manager socket. */
public final class SharedMicrogWorker {
    private static native boolean enabled();
    private static native String receive();
    private static native boolean send(int type, long channel);
    private static native boolean event(int type, long channel, int first, int second, int third);
    private static native void touch(long channel, int phase, int pointer, int x, int y);
    private static native void back(long channel);
    private static native void text(long channel, String value);
    private static native void textKey(long channel, int key);
    private static native void keyboardClosed(long channel);
    private static native void keyboardHeight(long channel, int height);
    private static final Object replies = new Object();
    private static boolean active;
    private static final Set<Activity> advancedLoginActivities =
        Collections.newSetFromMap(new WeakHashMap<Activity, Boolean>());

    private SharedMicrogWorker() {}

    static boolean showUi(long channel) { return event(8, channel, 0, 0, 0); }
    static void dismissUi(long channel) { event(9, channel, 0, 0, 0); }

    public static void activityResumed(Activity activity) {
        if (!active || activity == null ||
                !"org.microg.gms.auth.login.LoginActivity".equals(activity.getClass().getName()))
            return;
        synchronized (advancedLoginActivities) {
            if (!advancedLoginActivities.add(activity)) return;
        }
        try {
            Method next = activity.getClass().getDeclaredMethod("onNextButtonClicked");
            next.setAccessible(true);
            next.invoke(activity);
        } catch (ReflectiveOperationException e) {
            synchronized (advancedLoginActivities) { advancedLoginActivities.remove(activity); }
            System.err.println("Shared microG LoginActivity could not advance: " + e.getClass().getSimpleName());
        } catch (RuntimeException e) {
            synchronized (advancedLoginActivities) { advancedLoginActivities.remove(activity); }
            System.err.println("Shared microG LoginActivity advance failed");
        }
    }

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
                } else if (type == 12 && fields.length == 6) {
                    touch(channel, Integer.parseInt(fields[2]), Integer.parseInt(fields[3]),
                          Integer.parseInt(fields[4]), Integer.parseInt(fields[5]));
                } else if (type == 13) {
                    back(channel);
                } else if (type == 14) {
                    String[] textFields = message.split(" ", 3);
                    if (textFields.length != 3 || textFields[2].isEmpty())
                        throw new IOException("Invalid worker text input");
                    text(channel, textFields[2]);
                } else if (type == 15 && fields.length == 3) {
                    textKey(channel, Integer.parseInt(fields[2]));
                } else if (type == 16) {
                    keyboardClosed(channel);
                } else if (type == 17 && fields.length == 3) {
                    keyboardHeight(channel, Integer.parseInt(fields[2]));
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
