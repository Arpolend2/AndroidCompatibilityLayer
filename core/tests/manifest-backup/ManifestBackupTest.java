import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import java.io.File;

public final class ManifestBackupTest {
    private static void check(boolean actual, boolean expected, String description) {
        if (actual != expected) throw new AssertionError(description + ": expected " + expected + ", got " + actual);
    }
    private static boolean backup(ApplicationInfo app) {
        if (app == null) throw new AssertionError("missing ApplicationInfo");
        return (app.flags & ApplicationInfo.FLAG_ALLOW_BACKUP) != 0;
    }
    public static void main(String[] args) throws Exception {
        boolean expected = Boolean.parseBoolean(args[1]);
        PackageManager pm = PackageManager.self();
        ApplicationInfo own = pm.getApplicationInfo("megler.test.backup", 0);
        PackageInfo installed = pm.getPackageInfo("megler.test.backup", 0);
        check(backup(own), expected, "getApplicationInfo");
        check(backup(installed.applicationInfo), expected, "getPackageInfo");
        if (installed.installLocation != 1) throw new AssertionError("installed installLocation");
        String[] cases = { "false", "true", "default", "reference-false", "reference-true" };
        boolean[] values = { false, true, true, false, true };
        for (int i = 0; i < cases.length; i++) {
            PackageInfo archive = pm.getPackageArchiveInfo(new File(args[0], cases[i] + ".apk").getPath(), 0);
            if (archive == null) throw new AssertionError("missing archive " + cases[i]);
            check(backup(archive.applicationInfo), values[i], "archive " + cases[i]);
            if (archive.installLocation != 1) throw new AssertionError("archive installLocation " + cases[i]);
        }
        check(backup(pm.getApplicationInfo("megler.test.backup", 0)), expected, "installed after archives");
        System.out.println("manifest backup API checks passed for " + args[2]);
    }
}
