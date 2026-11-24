package android.system;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.util.IdentityHashMap;

public final class Os {
    private static final IdentityHashMap<FileDescriptor, FileChannel> channels = new IdentityHashMap<>();
    public static void register(FileDescriptor fd, FileChannel channel) { channels.put(fd, channel); }
    public static long lseek(FileDescriptor fd, long offset, int whence) throws IOException {
        FileChannel channel = channels.get(fd);
        if (channel == null || whence != 0 || offset < 0) throw new IOException("invalid seek");
        channel.position(offset);
        return channel.position();
    }
}
