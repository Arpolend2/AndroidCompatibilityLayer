package megler;

public final class Native {
    public static String packageName() { return System.getProperty("megler.test.package", "com.google.android.gms"); }
    public static long uptimeNanos() { return System.nanoTime(); }
    public static void threadNice(int priority) { throw new UnsupportedOperationException(); }
    public static void exit() { throw new UnsupportedOperationException(); }
}
