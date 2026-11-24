package megler;

public final class Native {
    public static final java.util.concurrent.atomic.AtomicInteger accountWarnings =
        new java.util.concurrent.atomic.AtomicInteger();
    private Native() {}
    public static void nativeLooperPrepare() {}
    public static boolean nativeLooperPoll() { return false; }
    public static void threadNice(int priority) {}
    public static long uptimeNanos() { return System.nanoTime(); }
    public static String dataDir() { return System.getProperty("fixture.dataDir"); }
    public static String packageName() { return "fixture.client"; }
    public static void log(int priority, String tag, String message) {
        if (priority >= 5 && "AccountManager".equals(tag)) accountWarnings.incrementAndGet();
        System.err.println("fixture log: " + tag);
    }
    public static boolean flag(String name) { return false; }
}
