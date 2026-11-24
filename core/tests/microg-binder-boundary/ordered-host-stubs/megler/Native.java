package megler;

public final class Native {
    private static volatile String packageName = "com.google.android.gms";
    public static void nativeLooperPrepare() {}
    public static boolean nativeLooperPoll() { return false; }
    public static long uptimeNanos() { return System.nanoTime(); }
    public static String packageName() { return packageName; }
    public static void setPackageName(String value) { packageName = value; }
}
