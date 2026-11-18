package megler;

public final class NetworkNative {
    public static volatile int snapshot;
    public static int state() { return snapshot; }
}
