package megler;

import java.util.HashMap;

/** Manager-supplied identities for the shared microG worker and its clients. */
public final class SharedMicrogIdentity {
    private static final HashMap<Long, Entry> channels = new HashMap<>();
    private static final HashMap<Integer, Entry> byUid = new HashMap<>();
    private static final HashMap<String, Entry> byPackage = new HashMap<>();
    private static volatile boolean worker;
    private static volatile int clientUid = -1;

    private static final class Entry {
        final String packageName;
        final int uid;
        int references;
        Entry(String packageName, int uid) {
            this.packageName = packageName;
            this.uid = uid;
        }
    }

    private SharedMicrogIdentity() {}

    static synchronized void startWorker() {
        if (worker || clientUid >= 0) throw new IllegalStateException("Shared microG identity already started");
        worker = true;
    }

    static synchronized void startClient(int uid, String managerPackage) {
        if (worker || clientUid >= 0) throw new IllegalStateException("Shared microG identity already started");
        if (uid < 11000 || uid > 19999 || managerPackage == null ||
                !managerPackage.equals(Native.packageName()))
            throw new IllegalArgumentException("Invalid shared microG client identity");
        clientUid = uid;
    }

    static synchronized void add(long channel, String packageName, int uid) {
        if (!worker || channel == 0 || packageName == null || packageName.isEmpty() ||
                uid < 11000 || uid > 19999 || channels.containsKey(channel))
            throw new IllegalArgumentException("Invalid shared microG identity");
        if ("android".equals(packageName) || "com.google.android.gms".equals(packageName) ||
                "com.google.android.gsf".equals(packageName) || "com.android.vending".equals(packageName) ||
                "com.google.android.webview".equals(packageName) || packageName.startsWith("com.android.webview"))
            throw new IllegalArgumentException("Reserved shared microG package");
        Entry uidEntry = byUid.get(uid);
        Entry packageEntry = byPackage.get(packageName);
        if ((uidEntry != null && !uidEntry.packageName.equals(packageName)) ||
                (packageEntry != null && packageEntry.uid != uid))
            throw new IllegalArgumentException("Conflicting shared microG identity");
        Entry entry = uidEntry != null ? uidEntry : new Entry(packageName, uid);
        channels.put(channel, entry);
        entry.references++;
        byUid.put(uid, entry);
        byPackage.put(packageName, entry);
    }

    static synchronized void remove(long channel) {
        Entry entry = channels.remove(channel);
        if (entry == null) return;
        --entry.references;
    }

    public static boolean isWorker() { return worker; }
    public static boolean isClient() { return clientUid >= 0; }
    public static int clientUid() { return clientUid; }

    public static synchronized String packageForUid(int uid) {
        Entry entry = byUid.get(uid);
        return entry == null || entry.references == 0 ? null : entry.packageName;
    }

    public static synchronized int uidForPackage(String packageName) {
        Entry entry = byPackage.get(packageName);
        return entry == null || entry.references == 0 ? -1 : entry.uid;
    }
}
