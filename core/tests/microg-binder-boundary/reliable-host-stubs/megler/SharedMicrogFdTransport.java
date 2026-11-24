package megler;

public final class SharedMicrogFdTransport {
    public static boolean available() { return true; }
    public static int[] createPipe() { return android.system.Os.pair(); }
    public static int[] createSocketPair(int type) { return android.system.Os.pair(); }
}
