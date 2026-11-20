package megler;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Process;
import java.util.ArrayList;
import java.util.Set;

/** Recognizes only ProcessPhoenix's default-launcher rebirth for Aurora Store. */
final class StorePhoenix {
    private StorePhoenix() {}

    static boolean request(Intent intent) {
        String pkg = Native.packageName();
        if (!"com.aurora.store".equals(pkg) || intent == null) return false;
        ComponentName component = intent.getComponent();
        if (component == null) return false;
        if (!matches(pkg, intent, Manifest.activity(component.getClassName()), Manifest.launcher(), Process.myPid()))
            return false;
        if (!Native.storeRestart()) {
            android.util.Log.e("Megler", "Aurora Store restart channel unavailable");
            return false;
        }
        Native.exit();
        return true;
    }

    static boolean matches(String pkg, Intent intent, Manifest.Component phoenix,
                           Manifest.Component launcher, int pid) {
        if (!"com.aurora.store".equals(pkg) || intent == null) return false;
        ComponentName component = intent.getComponent();
        if (component == null || !pkg.equals(component.getPackageName())) return false;
        String name = component.getClassName();
        if (!"com.jakewharton.processphoenix.ProcessPhoenix".equals(name)
                && !"com.jakewharton.processphoenix.PhoenixActivity".equals(name)) return false;
        if (phoenix == null || launcher == null
                || !name.equals(phoenix.name)
                || !(":phoenix".equals(phoenix.process) || "com.aurora.store:phoenix".equals(phoenix.process)))
            return false;
        if (intent.hasExtra("phoenix_main_process_pid")
                && intent.getIntExtra("phoenix_main_process_pid", -1) != pid) return false;
        ArrayList<?> intents = intent.getParcelableArrayListExtra("phoenix_restart_intents");
        if (intents == null || intents.size() != 1 || !(intents.get(0) instanceof Intent)) return false;
        Intent next = (Intent) intents.get(0);
        ComponentName target = next.getComponent();
        if (target == null || !"com.aurora.store".equals(target.getPackageName())
                || !launcher.name.equals(target.getClassName())
                || !Intent.ACTION_MAIN.equals(next.getAction()) || next.getData() != null
                || next.getType() != null || next.getSelector() != null || next.getClipData() != null
                || (next.getExtras() != null && !next.getExtras().isEmpty())) return false;
        Set<String> categories = next.getCategories();
        if (categories != null) for (String category : categories)
            if (!Intent.CATEGORY_LAUNCHER.equals(category) && !Intent.CATEGORY_DEFAULT.equals(category)) return false;
        return true;
    }
}
