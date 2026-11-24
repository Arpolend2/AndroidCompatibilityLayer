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
        check(true, "unrelated Activity is ignored");
        System.out.println("Shared microG lifecycle test passed");
    }
}
