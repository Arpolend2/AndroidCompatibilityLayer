package megler;

import android.content.pm.PackageManager;
import java.util.Arrays;

public final class SharedMicrogIdentityTest {
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
        check(!SharedMicrogIdentity.isWorker(), "ordinary process mode");
        reject(() -> SharedMicrogIdentity.add(1, "example.first", 11000));
        SharedMicrogIdentity.startWorker();
        PackageManager pm = new PackageManager();
        SharedMicrogIdentity.add(1, "example.first", 11000);
        SharedMicrogIdentity.add(2, "example.first", 11000);
        SharedMicrogIdentity.add(3, "example.second", 11001);
        check(Arrays.equals(pm.getPackagesForUid(11000), new String[] { "example.first" }), "first package");
        check(Arrays.equals(pm.getPackagesForUid(11001), new String[] { "example.second" }), "second package");
        check(pm.getPackageUid("example.first", 0) == 11000, "package uid");
        check("example.second".equals(pm.getNameForUid(11001)), "name for uid");
        check(pm.getPackagesForUid(11002) == null, "unknown uid");
        reject(() -> SharedMicrogIdentity.add(1, "example.first", 11000));
        reject(() -> SharedMicrogIdentity.add(4, "example.second", 11000));
        reject(() -> SharedMicrogIdentity.add(4, "example.first", 11002));
        reject(() -> SharedMicrogIdentity.add(4, "com.google.android.gms", 11002));
        reject(() -> SharedMicrogIdentity.add(4, "example.third", 10100));
        SharedMicrogIdentity.remove(1);
        check(pm.getPackageUid("example.first", 0) == 11000, "other channel keeps identity");
        SharedMicrogIdentity.remove(2);
        SharedMicrogIdentity.remove(2);
        check(pm.getPackagesForUid(11000) == null && pm.getNameForUid(11000) == null, "last channel revoked");
        try {
            pm.getPackageUid("example.first", 0);
            throw new AssertionError("revoked package uid");
        } catch (PackageManager.NameNotFoundException expected) {}
        reject(() -> SharedMicrogIdentity.add(4, "example.third", 11000));
        reject(() -> SharedMicrogIdentity.add(4, "example.first", 11002));
        SharedMicrogIdentity.add(4, "example.first", 11000);
        check(pm.getPackageUid("example.first", 0) == 11000, "same identity can reconnect");
        SharedMicrogIdentity.remove(4);
        check(pm.getPackageUid("example.second", 0) == 11001, "other identity unaffected");
        SharedMicrogIdentity.remove(3);
        System.out.println("Shared microG identity tests passed");
    }
}
