package megler;

import android.app.Activity;
import java.lang.reflect.Field;

public final class SharedMicrogWorkerLifecycleTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void setActive() throws Exception {
        Field active = SharedMicrogWorker.class.getDeclaredField("active");
        active.setAccessible(true);
        active.setBoolean(null, true);
    }

    public static void main(String[] args) throws Exception {
        setActive();
        org.microg.gms.auth.login.LoginActivity login =
            new org.microg.gms.auth.login.LoginActivity();
        SharedMicrogWorker.activityResumed(login);
        check(login.nextCalls == 1, "LoginActivity advances once");
        SharedMicrogWorker.activityResumed(login);
        check(login.nextCalls == 1, "LoginActivity advance is one-shot");

        Activity other = new Activity();
        SharedMicrogWorker.activityResumed(other);
        check(login.nextCalls == 1, "unrelated Activity is ignored");

        Activity first = new Activity();
        Activity child = new Activity();
        Activity replacement = new Activity();
        Activity stranger = new Activity();
        SharedMicrogWorker.activityCreated(first, null, 11, 1);
        SharedMicrogWorker.activityCreated(child, first, 0, 0);
        SharedMicrogWorker.activityReplaced(child, replacement);
        check(SharedMicrogWorker.frameOwnedBy(first, 11, 1), "root belongs to its session");
        check(SharedMicrogWorker.frameOwnedBy(child, 11, 1), "child inherits its source session");
        check(SharedMicrogWorker.frameOwnedBy(replacement, 11, 1), "recreated activity retains ownership");
        check(!SharedMicrogWorker.frameOwnedBy(first, 11, 2), "previous generation is rejected");
        check(!SharedMicrogWorker.frameOwnedBy(first, 22, 1), "another channel is rejected");
        check(!SharedMicrogWorker.frameOwnedBy(stranger, 11, 1), "unowned window is rejected");
        check(!SharedMicrogWorker.frameOwnedBy(null, 11, 1), "unknown window owner is rejected");
        System.out.println("Shared microG lifecycle test passed");
    }
}
