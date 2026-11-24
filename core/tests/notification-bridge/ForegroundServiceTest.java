import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.os.IBinder;
import megler.Manifest;
import megler.Native;
import megler.Permissions;

public final class ForegroundServiceTest {
    private static final class Manager extends NotificationManager {
        int posts, cancels, id, canceledId;
        Notification notification;
        @Override public void notify(int id, Notification n) {
            posts++;
            this.id = id;
            notification = n;
        }
        @Override public void cancel(int id) { cancels++; canceledId = id; }
    }

    private static final class ForegroundService extends Service {
        boolean destroyed, failDestroy, detachDuringDestroy;
        @Override public IBinder onBind(Intent intent) { return null; }
        @Override public void onDestroy() {
            destroyed = true;
            if (detachDuringDestroy) stopForeground(STOP_FOREGROUND_DETACH);
            if (failDestroy) throw new IllegalStateException("destroy failed");
        }
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        Manager manager = new Manager();
        ForegroundService service = new ForegroundService();
        service.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) {
                return Context.NOTIFICATION_SERVICE.equals(name) ? manager : null;
            }
        });
        Notification n = new Notification.Builder(null, "downloads")
                .setContentTitle("Downloading").build();
        service.startForeground(200, n, 4);
        check(Native.logMessages.size() == 1 &&
              "ACL background: foreground service started".equals(Native.logMessages.get(0)),
              "foreground start diagnostic");
        check(manager.posts == 1 && manager.id == 200 && manager.notification == n &&
              (n.flags & Notification.FLAG_FOREGROUND_SERVICE) != 0 &&
              (n.flags & Notification.FLAG_ONGOING_EVENT) != 0 &&
              service.getForegroundServiceType() == 4, "initial foreground notification");
        service.startForeground(200, n);
        check(Native.logMessages.size() == 1, "foreground update does not repeat start diagnostic");
        check(manager.posts == 2 && manager.cancels == 0 &&
              service.getForegroundServiceType() == 0, "same id updated");
        service.startForeground(201, n, 2);
        check(manager.posts == 3 && manager.id == 201 && manager.cancels == 1 &&
              manager.canceledId == 200, "changed id replaces foreground notification");
        service.stopForeground(Service.STOP_FOREGROUND_REMOVE);
        check(Native.logMessages.size() == 2 &&
              "ACL background: foreground service stopped".equals(Native.logMessages.get(1)),
              "foreground stop diagnostic");
        check(manager.cancels == 2 && manager.canceledId == 201 &&
              service.getForegroundServiceType() == 0, "remove foreground notification");
        service.startForeground(202, n);
        service.stopForeground(Service.STOP_FOREGROUND_DETACH);
        service.meglerDestroy();
        check(manager.cancels == 2, "detached notification remains");
        service.startForeground(203, n);
        service.meglerDestroy();
        check(service.destroyed && manager.cancels == 3 && manager.canceledId == 203,
              "system destruction removes foreground notification without super.onDestroy");
        service.startForeground(204, n);
        service.stopForeground(false);
        check(manager.cancels == 3, "legacy stop retains notification");
        service.meglerDestroy();
        check(manager.cancels == 4 && manager.canceledId == 204, "legacy notification removed on destroy");
        service.startForeground(205, n);
        service.failDestroy = true;
        try {
            service.meglerDestroy();
            throw new AssertionError("onDestroy failure swallowed");
        } catch (IllegalStateException expected) {}
        check(manager.cancels == 5 && manager.canceledId == 205, "cleanup after onDestroy failure");
        service.failDestroy = false;
        service.detachDuringDestroy = true;
        service.startForeground(206, n);
        service.meglerDestroy();
        check(manager.cancels == 5, "notification detached during onDestroy remains");
        try {
            service.startForeground(0, n);
            throw new AssertionError("zero id accepted");
        } catch (IllegalArgumentException expected) {}

        Manifest.targetSdk = 28;
        Permissions.state = 0;
        NotificationManager bridge = new NotificationManager() {
            @Override public NotificationChannel getNotificationChannel(String id) {
                NotificationChannel channel = new NotificationChannel("downloads", "Downloads", IMPORTANCE_MIN);
                channel.setSound(null, null);
                return channel;
            }
            @Override public NotificationChannelGroup getNotificationChannelGroup(String id) { return null; }
        };
        ForegroundService bridged = new ForegroundService();
        bridged.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return bridge; }
        });
        bridged.startForeground(200, n);
        check(Native.calls == 1 && Native.id == 200 && Native.operation == 1 &&
              Native.importance == NotificationManager.IMPORTANCE_MIN && Native.silent &&
              "Downloading".equals(Native.title) &&
              (Native.flags & Notification.FLAG_FOREGROUND_SERVICE) != 0,
              "foreground notification reaches classified bridge");
        bridged.stopForeground(true);
        check(Native.calls == 2 && Native.operation == 2 && Native.id == 200,
              "foreground removal reaches bridge");

        Manifest.targetSdk = 25;
        NotificationManager firstManager = new NotificationManager();
        NotificationManager secondManager = new NotificationManager();
        ForegroundService first = new ForegroundService();
        ForegroundService second = new ForegroundService();
        first.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return firstManager; }
        });
        second.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return secondManager; }
        });
        first.startForeground(300, n);
        firstManager.notify("tag", 300, n);
        firstManager.notify(301, n);
        secondManager.cancel(300);
        check(Native.active.contains("null:300"), "cancel cannot remove active foreground notification");
        secondManager.notify(300, new Notification.Builder(null).setContentTitle("updated").build());
        check((Native.flags & Notification.FLAG_FOREGROUND_SERVICE) != 0 &&
              (Native.flags & Notification.FLAG_ONGOING_EVENT) != 0,
              "ordinary foreground update retains system flags");
        secondManager.cancelAll();
        check(Native.active.contains("null:300") && !Native.active.contains("tag:tag:300") &&
              !Native.active.contains("null:301"), "cancelAll retains only foreground notification");
        second.startForeground(300, n);
        first.stopForeground(true);
        check(Native.active.contains("null:300"), "other service still owns foreground notification");
        first.startForeground(300, n);
        first.startForeground(304, n);
        check(Native.active.contains("null:300") && Native.active.contains("null:304"),
              "switching id keeps another service's notification");
        first.stopForeground(true);
        second.startForeground(302, n);
        check(!Native.active.contains("null:300") && Native.active.contains("null:302"),
              "switching foreground id removes old notification");
        second.stopForeground(Service.STOP_FOREGROUND_DETACH);
        secondManager.cancel(302);
        check(!Native.active.contains("null:302"), "detached notification can be cancelled");
        first.startForeground(303, n);
        second.startForeground(303, n);
        first.meglerDestroy();
        check(Native.active.contains("null:303"), "destroy keeps notification owned by another service");
        second.meglerDestroy();
        check(!Native.active.contains("null:303"), "last service destruction removes notification");

        ForegroundService failing = new ForegroundService();
        NotificationManager failingManager = new NotificationManager() {
            @Override public void notify(int id, Notification notification) {
                throw new IllegalStateException("publication failed");
            }
        };
        failing.meglerAttach(new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return failingManager; }
        });
        int logCount = Native.logMessages.size();
        try {
            failing.startForeground(305, n);
            throw new AssertionError("publication failure swallowed");
        } catch (IllegalStateException expected) {}
        check(Native.logMessages.size() == logCount, "failed start is not logged as active");
        firstManager.notify(305, n);
        firstManager.cancel(305);
        check(!Native.active.contains("null:305"), "failed start leaves no foreground protection");
    }
}
