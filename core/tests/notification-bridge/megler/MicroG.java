package megler;

import android.app.Service;

public final class MicroG {
    public static int stopSelf(Service service, int id) { return -1; }
    public static void onMain(Runnable runnable) { runnable.run(); }
}
