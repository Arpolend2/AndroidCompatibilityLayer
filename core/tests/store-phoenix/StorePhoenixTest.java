package megler;

import android.content.ComponentName;
import android.content.Intent;
import java.util.ArrayList;

public final class StorePhoenixTest {
    private static final String PKG = "com.aurora.store";
    private static final String OLD = "com.jakewharton.processphoenix.ProcessPhoenix";
    private static final String NEW = "com.jakewharton.processphoenix.PhoenixActivity";
    private static final String TARGET = "com.aurora.store.Main";

    private static Intent restart(String cls, int pid) {
        Intent target = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            .setComponent(new ComponentName(PKG, TARGET));
        ArrayList<Intent> intents = new ArrayList<>();
        intents.add(target);
        Intent result = new Intent().setComponent(new ComponentName(PKG, cls));
        result.putParcelableArrayListExtra("phoenix_restart_intents", intents);
        if (pid >= 0) result.putExtra("phoenix_main_process_pid", pid);
        return result;
    }

    private static void check(boolean value, String name) {
        if (!value) throw new AssertionError(name);
    }

    public static void main(String[] args) {
        Manifest.Component phoenix = new Manifest.Component();
        Manifest.Component launcher = new Manifest.Component();
        launcher.name = TARGET;
        phoenix.process = ":phoenix";
        phoenix.name = OLD;
        Intent old = restart(OLD, -1);
        check(StorePhoenix.matches(PKG, old, phoenix, launcher, 4242), "2.0 rebirth");
        phoenix.name = NEW;
        Intent current = restart(NEW, 4242);
        check(StorePhoenix.matches(PKG, current, phoenix, launcher, 4242), "current rebirth");
        check(!StorePhoenix.matches("other.app", current, phoenix, launcher, 4242), "other app");
        check(!StorePhoenix.matches(PKG, current, phoenix, launcher, 123), "wrong pid");
        phoenix.process = null;
        check(!StorePhoenix.matches(PKG, current, phoenix, launcher, 4242), "not separate process");
        phoenix.process = ":phoenix";
        current.<Intent>getParcelableArrayListExtra("phoenix_restart_intents").get(0).putExtra("custom", "data");
        check(!StorePhoenix.matches(PKG, current, phoenix, launcher, 4242), "custom launch intent");
        Intent unknown = restart("other.Activity", -1);
        check(!StorePhoenix.matches(PKG, unknown, phoenix, launcher, 4242), "normal activity");
        Intent multiple = restart(NEW, 4242);
        multiple.<Intent>getParcelableArrayListExtra("phoenix_restart_intents").add(new Intent());
        check(!StorePhoenix.matches(PKG, multiple, phoenix, launcher, 4242), "multiple activities");
    }
}
