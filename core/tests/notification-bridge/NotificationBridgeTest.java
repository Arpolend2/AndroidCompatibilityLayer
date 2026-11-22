import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.content.ContextWrapper;
import megler.Manifest;
import megler.Native;
import megler.Permissions;

public final class NotificationBridgeTest {
    private static final class Manager extends NotificationManager {
        NotificationChannel channel;
        NotificationChannelGroup group;
        @Override public NotificationChannel getNotificationChannel(String id) { return channel; }
        @Override public NotificationChannelGroup getNotificationChannelGroup(String id) { return group; }
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        Manager manager = new Manager();
        Notification n = new Notification.Builder(null, "messages")
                .setContentTitle("Title").setContentText("Body").build();
        manager.channel = new NotificationChannel("messages", "Messages", NotificationManager.IMPORTANCE_DEFAULT);
        Permissions.permission = -1;
        manager.notify("tag", 7, n);
        check(Native.calls == 0, "denied permission");
        Permissions.permission = 0;
        manager.channel = null;
        manager.notify("tag", 7, n);
        check(Native.calls == 0, "missing channel");
        manager.channel = new NotificationChannel("messages", "Messages", NotificationManager.IMPORTANCE_NONE);
        manager.notify("tag", 7, n);
        check(Native.calls == 0, "blocked channel");
        manager.channel = new NotificationChannel("messages", "Messages", NotificationManager.IMPORTANCE_DEFAULT);
        manager.group = new NotificationChannelGroup("group", "Group");
        manager.group.setBlocked(true);
        manager.notify("tag", 7, n);
        check(Native.calls == 0, "blocked group");
        manager.group = null;
        manager.notify("tag", 7, n);
        check(Native.calls == 1 && Native.operation == 1 && Native.id == 7 &&
              "tag".equals(Native.tag) && "Title".equals(Native.title) && "Body".equals(Native.body),
              "notification delivered");
        manager.cancel("tag", 7);
        check(Native.calls == 2 && Native.operation == 2, "cancel");
        manager.cancelAll();
        check(Native.calls == 3 && Native.operation == 3, "cancel all");
        NotificationManager foreign = new NotificationManager(new ContextWrapper(null) {
            @Override public String getPackageName() { return "org.example.other"; }
        });
        foreign.notify(7, n);
        foreign.cancelAll();
        check(Native.calls == 3 && !foreign.areNotificationsEnabled(), "foreign package isolated");
        Manifest.targetSdk = 25;
        Permissions.state = 3;
        check(!manager.areNotificationsEnabled(), "legacy app denied");
        Permissions.state = 0;
        manager.channel = null;
        manager.notify(8, n);
        check(Native.calls == 4 && Native.id == 8, "legacy app without channel");
        Notification.Builder progress = new Notification.Builder(null, "messages")
                .setOnlyAlertOnce(true).setProgress(100, 25, false);
        manager.notify(9, progress.build());
        check(Native.calls == 5 && (Native.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0 &&
              Native.max == 100 && Native.progress == 25 && !Native.indeterminate,
              "progress metadata delivered");
        progress.setOnlyAlertOnce(false).setProgress(0, 0, true);
        manager.notify(9, progress.build());
        check(Native.calls == 6 && (Native.flags & Notification.FLAG_ONLY_ALERT_ONCE) == 0 &&
              Native.max == 0 && Native.indeterminate, "indeterminate progress and flag reset");
        progress.setProgress(0, 0, false);
        manager.notify(9, progress.build());
        check(Native.calls == 7 && Native.max == 0 && !Native.indeterminate, "progress cleared");
    }
}
