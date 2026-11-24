package android.system;

public final class ErrnoException extends Exception {
    public final int errno;
    public ErrnoException(int errno) { super("secret"); this.errno = errno; }
}
