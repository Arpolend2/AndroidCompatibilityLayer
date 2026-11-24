package megler;

public final class Native {
    public static int calls, operation, id, flags, max, progress, importance;
    public static boolean indeterminate, silent, classifiedUnavailable;
    public static int legacyCalls, warnings;
    public static String tag, title, body;
    public static final java.util.Set<String> active = new java.util.HashSet<>();
    public static final java.util.List<String> logMessages = new java.util.ArrayList<>();
    public static String packageName() { return "org.example.app"; }
    public static void log(int priority, String tag, String message) {
        if (priority >= 5) warnings++;
        logMessages.add(message);
    }
    public static boolean notification(int op, String t, int i, String summary, String text) {
        calls++;
        operation = op;
        id = i;
        tag = t;
        title = summary;
        body = text;
        String key = (t == null ? "null:" : "tag:" + t + ":") + i;
        if (op == 1) active.add(key);
        else if (op == 2) active.remove(key);
        else if (op == 3) active.clear();
        return true;
    }
    public static boolean notificationClassified(String t, int i, String summary, String text,
                                                 int f, int m, int p, boolean ind, int imp, boolean quiet) {
        if (classifiedUnavailable) throw new UnsatisfiedLinkError("old runtime");
        flags = f;
        max = m;
        progress = p;
        indeterminate = ind;
        importance = imp;
        silent = quiet;
        return notification(1, t, i, summary, text);
    }
    public static boolean notificationDetailed(String t, int i, String summary, String text,
                                               int f, int m, int p, boolean ind) {
        legacyCalls++;
        return notification(1, t, i, summary, text);
    }
}
