package android.system;

import java.io.FileDescriptor;
import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.HashMap;

public final class Os {
    private static final class End {
        End peer;
        int refs = 1;
        final ArrayDeque<byte[]> packets = new ArrayDeque<>();
    }
    private static final HashMap<Integer, End> ends = new HashMap<>();
    private static int next = 100;
    private static int dupBeforeFailure = -1;
    public static synchronized int openCount() { return ends.size(); }
    public static synchronized void failDupAfter(int count) { dupBeforeFailure = count; }
    private static int number(FileDescriptor fd) {
        try {
            Field field = FileDescriptor.class.getDeclaredField("fd");
            field.setAccessible(true);
            return field.getInt(fd);
        } catch (ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    private static FileDescriptor descriptor(int number) {
        try {
            FileDescriptor fd = new FileDescriptor();
            Field field = FileDescriptor.class.getDeclaredField("fd");
            field.setAccessible(true);
            field.setInt(fd, number);
            return fd;
        } catch (ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    public static synchronized int[] pair() {
        End a = new End(), b = new End();
        a.peer = b; b.peer = a;
        int x = next++, y = next++;
        ends.put(x, a); ends.put(y, b);
        return new int[] {x, y};
    }
    public static synchronized FileDescriptor dup(FileDescriptor fd) throws ErrnoException {
        if (dupBeforeFailure == 0) { dupBeforeFailure = -1; throw new ErrnoException(24); }
        if (dupBeforeFailure > 0) --dupBeforeFailure;
        End end = ends.get(number(fd));
        if (end == null) throw new ErrnoException(9);
        ++end.refs;
        int copy = next++;
        ends.put(copy, end);
        return descriptor(copy);
    }
    public static synchronized void close(FileDescriptor fd) throws ErrnoException {
        End end = ends.remove(number(fd));
        if (end == null) throw new ErrnoException(9);
        --end.refs;
        try {
            Field field = FileDescriptor.class.getDeclaredField("fd");
            field.setAccessible(true); field.setInt(fd, -1);
        } catch (ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    public static int fcntlInt(FileDescriptor fd, int command, int value) { return 0; }
    public static synchronized int write(FileDescriptor fd, byte[] bytes, int off, int len) throws ErrnoException {
        End end = ends.get(number(fd));
        if (end == null) throw new ErrnoException(9);
        if (end.peer.refs == 0) throw new ErrnoException(32);
        byte[] copy = new byte[len];
        System.arraycopy(bytes, off, copy, 0, len);
        end.peer.packets.add(copy);
        return len;
    }
    public static synchronized int read(FileDescriptor fd, byte[] bytes, int off, int len) throws ErrnoException {
        End end = ends.get(number(fd));
        if (end == null) throw new ErrnoException(9);
        byte[] packet = end.packets.poll();
        if (packet == null) {
            if (end.peer.refs == 0) return 0;
            throw new ErrnoException(11);
        }
        int count = Math.min(len, packet.length);
        System.arraycopy(packet, 0, bytes, off, count);
        return count;
    }
}
