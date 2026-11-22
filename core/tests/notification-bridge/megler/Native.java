package megler;

public final class Native {
    public static int calls, operation, id, flags, max, progress;
    public static boolean indeterminate;
    public static String tag, title, body;
    public static String packageName() { return "org.example.app"; }
    public static boolean notification(int op, String t, int i, String summary, String text) {
        calls++;
        operation = op;
        id = i;
        tag = t;
        title = summary;
        body = text;
        return true;
    }
    public static boolean notificationDetailed(String t, int i, String summary, String text,
                                               int f, int m, int p, boolean ind) {
        flags = f;
        max = m;
        progress = p;
        indeterminate = ind;
        return notification(1, t, i, summary, text);
    }
}
