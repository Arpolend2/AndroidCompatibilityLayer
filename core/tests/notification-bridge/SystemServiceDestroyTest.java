import android.app.Notification;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ContextWrapper;
import android.content.Intent;
import android.os.IBinder;
import java.lang.reflect.Field;
import java.util.Map;
import megler.Services;

public final class SystemServiceDestroyTest {
    private static final class Manager extends NotificationManager {
        int canceled;
        @Override public void notify(int id, Notification n) {}
        @Override public void cancel(int id) { canceled = id; }
    }

    private static final class ForegroundService extends Service {
        boolean destroyed;
        @Override public IBinder onBind(Intent intent) { return null; }
        @Override public void onDestroy() { destroyed = true; }
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        Manager manager = new Manager();
        ForegroundService service = new ForegroundService();
        service.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return manager; }
        });
        service.startForeground(200, new Notification());
        Field running = Services.class.getDeclaredField("sRunning");
        running.setAccessible(true);
        ((Map<String, Service>) running.get(null)).put("test", service);
        Field lastStart = Services.class.getDeclaredField("sLastStart");
        lastStart.setAccessible(true);
        ((Map<String, Integer>) lastStart.get(null)).put("test", 2);

        if (Services.stopSelf(service, 1) || service.destroyed || manager.canceled != 0)
            throw new AssertionError("an older start must not destroy the service");
        if (!Services.stopSelf(service, 2) || !service.destroyed || manager.canceled != 200)
            throw new AssertionError("system stop must clean foreground after overridden onDestroy");
    }
}
