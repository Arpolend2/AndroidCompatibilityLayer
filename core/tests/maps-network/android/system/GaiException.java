package android.system;

public final class GaiException extends RuntimeException {
    public final int error;
    public GaiException(int error) { super("secret"); this.error = error; }
}
