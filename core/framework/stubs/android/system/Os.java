package android.system;

import java.io.FileDescriptor;

/** Compile-time shape; the Android root supplies this class at runtime. */
public final class Os {
    private Os() {}
    public static native void shutdown(FileDescriptor fd, int how) throws ErrnoException;
}
