package megler;

import android.content.pm.PackageManager;
import android.os.Process;
import java.util.Arrays;

public final class SharedMicrogClientIdentityTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void reject(Runnable action) {
        try {
            action.run();
            throw new AssertionError("Invalid identity accepted");
        } catch (IllegalArgumentException expected) {}
    }

    public static void main(String[] args) throws Exception {
        System.setProperty("megler.test.package", "example.client");
        check(Process.myUid() == 10100 && !SharedMicrogIdentity.isClient(), "legacy uid");
        reject(() -> SharedMicrogIdentity.startClient(10999, "example.client"));
        reject(() -> SharedMicrogIdentity.startClient(20000, "example.client"));
        reject(() -> SharedMicrogIdentity.startClient(11023, "example.other"));
        check(Process.myUid() == 10100, "failed bootstrap leaves uid unchanged");
        SharedMicrogIdentity.startClient(11023, "example.client");
        PackageManager pm = new PackageManager();
        check(Process.myUid() == 11023, "manager uid");
        check(pm.getPackageUid("example.client", 0) == 11023, "own package uid");
        check(Arrays.equals(pm.getPackagesForUid(11023), new String[] { "example.client" }), "own package list");
        check("example.client".equals(pm.getNameForUid(11023)), "own uid name");
        check(pm.getPackagesForUid(11024) == null && pm.getNameForUid(11024) == null, "unknown uid");
        PhoneApps.App other = new PhoneApps.App("example.other", null, "Other", "", 0, 11024);
        check(PhoneApps.applicationInfo(other).uid == -1, "other app uid is not guessed");
        try {
            pm.getPackageUid("example.other", 0);
            throw new AssertionError("unknown package uid guessed");
        } catch (PackageManager.NameNotFoundException expected) {}
        try {
            SharedMicrogIdentity.startClient(11024, "example.client");
            throw new AssertionError("client identity replaced");
        } catch (IllegalStateException expected) {}
        try {
            SharedMicrogIdentity.startWorker();
            throw new AssertionError("client became worker");
        } catch (IllegalStateException expected) {}
        System.out.println("Shared microG client identity tests passed");
    }
}
