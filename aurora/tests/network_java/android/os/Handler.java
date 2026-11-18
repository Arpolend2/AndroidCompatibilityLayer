package android.os;

import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

public class Handler {
    private static final ScheduledThreadPoolExecutor QUEUE = new ScheduledThreadPoolExecutor(1, r -> {
        Thread t = new Thread(r, "test-connectivity"); t.setDaemon(true); return t;
    });
    public Handler(Looper looper) {}
    public boolean post(Runnable r) { return postDelayed(r, 0); }
    public boolean postDelayed(Runnable r, long delay) { QUEUE.schedule(r, delay, TimeUnit.MILLISECONDS); return true; }
    public void removeCallbacks(Runnable r) { QUEUE.remove(r); }
}
