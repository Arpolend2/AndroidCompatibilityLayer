package megler;

import android.content.Context;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.app.Activity;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** Control messages and caller identities come only from the inherited manager socket. */
public final class SharedMicrogWorker {
    private static native boolean enabled();
    private static native String receive();
    private static native boolean send(int type, long channel);
    private static native long startUi(long channel);
    private static native boolean activateUi(long channel, long generation);
    private static native boolean cancelPendingUi(long channel, long generation);
    private static native long currentUiChannel();
    private static native long currentUiGeneration(long channel);
    private static native boolean authorizeFrame(long channel, long generation);
    private static native boolean finishUi(long channel, long generation);
    private static native void keyboardHeight(long channel, long generation, int height);
    private static native void resize(long channel, long generation, int width, int height, int density, int rotation);
    private static final Object replies = new Object();
    private static final ScheduledExecutorService cleanupWatchdog = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "megler-microg-ui-cleanup-watchdog");
        thread.setDaemon(true);
        return thread;
    });
    private static boolean active;
    private static long failedCleanupChannel;
    private static long failedCleanupGeneration;
    private static final Set<Activity> advancedLoginActivities =
        Collections.newSetFromMap(new WeakHashMap<Activity, Boolean>());
    private static final Map<Activity, FrameOwner> frameOwners = new WeakHashMap<>();

    private static final class FrameOwner {
        final long channel;
        final long generation;
        FrameOwner(long channel, long generation) {
            this.channel = channel;
            this.generation = generation;
        }
    }

    private SharedMicrogWorker() {}

    static long showUi(long channel) { return startUi(channel); }
    static boolean activateOnMain(long channel, long generation) {
        if (!activateUi(channel, generation)) return false;
        AppRunner.resetSharedMicrogInput();
        keyboardHeight(channel, generation, 0);
        ViewRoot.setImeHeight(0);
        return true;
    }
    static void dismissUi(long channel, long generation) {
        if (cancelPendingUi(channel, generation)) return;
        Runnable finish = () -> {
            if (uiMatches(channel, generation)) {
                if (failedCleanupChannel == channel && failedCleanupGeneration == generation) {
                    System.err.println("Shared microG UI cleanup previously failed; sign-in remains reserved");
                    return;
                }
                ScheduledFuture<?> warning = cleanupWatchdog.schedule(
                    () -> System.err.println("Shared microG UI cleanup is blocked; sign-in remains reserved"),
                    2, TimeUnit.SECONDS);
                try {
                    AppRunner.resetSharedMicrogInput();
                    keyboardHeight(channel, generation, 0);
                    ViewRoot.setImeHeight(0);
                    AppRunner.finishSharedMicrogActivities(channel, generation);
                } catch (RuntimeException | Error e) {
                    failedCleanupChannel = channel;
                    failedCleanupGeneration = generation;
                    System.err.println("Shared microG UI cleanup failed; sign-in remains reserved");
                    throw e;
                } finally {
                    warning.cancel(false);
                }
            }
            finishUi(channel, generation);
        };
        if (Looper.myLooper() == Looper.getMainLooper()) finish.run();
        else if (!AppRunner.postSharedMicrog(finish)) {
            System.err.println("Shared microG UI finish could not be scheduled");
        }
    }

    static boolean isActive() { return active; }

    static boolean uiMatches(long channel, long generation) {
        return active && channel != 0 && generation != 0 && currentUiGeneration(channel) == generation;
    }

    static void deliverInput(int type, long channel, long generation, String[] arguments) {
        if (!uiMatches(channel, generation)) return;
        if (type == 21) {
            resize(channel, generation, Integer.parseInt(arguments[3]),
                Integer.parseInt(arguments[4]), Integer.parseInt(arguments[5]),
                Integer.parseInt(arguments[6]));
        } else if (type == 17) {
            int height = Integer.parseInt(arguments[3]);
            keyboardHeight(channel, generation, height);
            ViewRoot.setImeHeight(height);
        } else if (ViewRoot.sharedMicrogOwnsInput(channel, generation)) {
            AppRunner.deliverSharedMicrogInput(type, arguments);
        }
    }

    static void activityCreated(Activity activity, Activity source, long channel, long generation) {
        if (!active) return;
        FrameOwner owner = channel != 0 && generation != 0
            ? new FrameOwner(channel, generation) : frameOwners.get(source);
        if (owner != null) frameOwners.put(activity, owner);
    }

    static void activityReplaced(Activity previous, Activity replacement) {
        if (!active) return;
        FrameOwner owner = frameOwners.get(previous);
        if (owner != null) frameOwners.put(replacement, owner);
    }

    static long frameChannel() { return active ? currentUiChannel() : 0; }
    static long frameGeneration(long channel) { return currentUiGeneration(channel); }
    static boolean frameOwnedBy(Activity activity, long channel, long generation) {
        FrameOwner owner = frameOwners.get(activity);
        return owner != null && owner.channel == channel && owner.generation == generation;
    }
    static boolean permitsActivityStart(Activity source) {
        if (!active || source == null) return true;
        long channel = frameChannel();
        return channel != 0 && frameOwnedBy(source, channel, frameGeneration(channel));
    }
    static boolean permitFrame(long channel, long generation) { return authorizeFrame(channel, generation); }
    static void suppressFrames() { authorizeFrame(0, 0); }

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
                } else if ((type >= 12 && type <= 17) || type == 21) {
                    if (fields.length < 3) throw new IOException("Invalid worker input");
                    long generation = Long.parseUnsignedLong(fields[2], 16);
                    String[] arguments = type == 14 ? message.split(" ", 4) : fields;
                    if ((type == 12 || type == 21) && arguments.length != 7 ||
                            (type == 13 || type == 16) && arguments.length != 3 ||
                            (type == 15 || type == 17) && arguments.length != 4 ||
                            type == 14 && (arguments.length != 4 || arguments[3].isEmpty()))
                        throw new IOException("Invalid worker input");
                    if (!AppRunner.postSharedMicrog(() -> deliverInput(type, channel, generation, arguments)))
                        throw new IOException("Cannot schedule shared microG input");
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
