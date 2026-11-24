package android.util;

public final class Log {
    public static final StringBuilder output = new StringBuilder();
    public static int i(String tag, String message) { output.append(message).append('\n'); return 0; }
    public static int w(String tag, String message) { output.append(message).append('\n'); return 0; }
}
