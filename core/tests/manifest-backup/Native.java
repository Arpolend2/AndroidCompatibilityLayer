package megler;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class Native {
    public static String apkPath() { return System.getProperty("test.apk"); }
    public static String packageName() { return "megler.test.backup"; }
    public static String dataDir() { return System.getProperty("java.io.tmpdir"); }
    public static String[] splitPaths() { return null; }
    public static String gmsPackage() { return null; }
    public static boolean flag(String name) { return false; }
    public static int screenWidth() { return 800; }
    public static int screenHeight() { return 480; }
    public static float density() { return 1; }
    public static boolean nightMode() { return false; }
    public static int accentColor() { return 0; }
    public static long uptimeNanos() { return System.nanoTime(); }
    public static void log(int priority, String tag, String message) { System.err.println(tag + ": " + message); }
    public static java.nio.ByteBuffer mapApkFile(int apk, String name) { return null; }
    public static byte[] readApkFile(int apk, String name) {
        if (apk != 0) return null;
        try (ZipFile zip = new ZipFile(apkPath())) {
            ZipEntry entry = zip.getEntry(name);
            if (entry == null) return null;
            try (InputStream in = zip.getInputStream(entry); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
                return out.toByteArray();
            }
        } catch (java.io.IOException e) {
            throw new IllegalStateException(e);
        }
    }
}
