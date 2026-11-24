package android.system;

public class ErrnoException extends Exception {
    public final int errno;
    public ErrnoException(int errno) { this.errno = errno; }
}
