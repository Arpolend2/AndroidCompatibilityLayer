package megler;
public class MediaCodecNative {
    public static boolean fail;
    public static boolean supported(String mime) { return true; }
    public static long create(String mime) { return 1; }
    public static boolean configure(long h, int w, int height, int rate, int channels, byte[] a, byte[] b, byte[] c) { return true; }
    public static int decode(long h, byte[] data, int length, long pts, int flags) { return fail ? -1 : 0; }
    public static long nextFrame(long h, long[] info) { return 0; }
    public static byte[] nextPcm(long h, long[] info) { return null; }
    public static int size(long h) { return 0; }
    public static void flush(long h) {}
    public static void destroy(long h) {}
    public static void freeFrame(long h) {}
}
