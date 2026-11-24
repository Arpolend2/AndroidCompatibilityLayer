package megler;

import java.io.IOException;

/** Native descriptor operations for the authenticated shared microG transport. */
public final class SharedMicrogFdTransport {
    private SharedMicrogFdTransport() {}

    public static native boolean available();
    public static native int[] createPipe() throws IOException;
    public static native int[] createSocketPair(int type) throws IOException;
    static native void sendMarker(int stream, int[] descriptors) throws IOException;
    static native int[] receiveMarker(int stream) throws IOException;
}
