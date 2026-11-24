package megler;

import android.app.Activity;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;

public final class SharedMicrogKeyboardOwnershipTest {
    private static int commits;
    private static native void configure(long channel, long generation);
    private static native int hides();
    private static native int shows();

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Activity activity() {
        return new Activity() {
            @Override public Object getSystemService(String name) { return new InputMethodManager(); }
        };
    }

    private static View window(Activity activity, long generation) throws Exception {
        SharedMicrogWorker.activityCreated(activity, null, 11, generation);
        View view = new View(activity) {
            @Override public InputConnection onCreateInputConnection(EditorInfo info) {
                return new BaseInputConnection(this, true) {
                    @Override public boolean commitText(CharSequence text, int position) {
                        commits++;
                        return true;
                    }
                };
            }
        };
        Constructor<ViewRoot> constructor = ViewRoot.class.getDeclaredConstructor(
            View.class, WindowManager.LayoutParams.class);
        constructor.setAccessible(true);
        ViewRoot root = constructor.newInstance(view, new WindowManager.LayoutParams());
        Field attachedRoot = View.class.getDeclaredField("mRoot");
        attachedRoot.setAccessible(true);
        attachedRoot.set(view, root);
        ViewRoot.roots().add(root);
        return view;
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Looper.prepareMainLooper();
        Field active = SharedMicrogWorker.class.getDeclaredField("active");
        active.setAccessible(true);
        active.setBoolean(null, true);
        configure(11, 1);
        View old = window(activity(), 1);
        check(InputMethods.show(old) && shows() == 1, "current window can show keyboard");
        Runnable oldInput = () -> SharedMicrogWorker.deliverInput(14, 11, 1,
            new String[] { "14", "b", "1", "fixture" });
        InputMethods.hide(old);
        check(hides() == 1, "current window can hide keyboard");

        configure(11, 2);
        View current = window(activity(), 2);
        check(!SharedMicrogWorker.permitsActivityStart((Activity) old.getContext()),
            "late activity start from old session is rejected");
        check(SharedMicrogWorker.permitsActivityStart((Activity) current.getContext()),
            "current activity can start a child");
        check(!InputMethods.show(old) && shows() == 1, "late show is rejected");
        InputMethods.hide(old);
        new InputMethodManager().hideSoftInputFromWindow(old.getWindowToken(), 0);
        InputMethods.hide();
        check(hides() == 1, "late or unattributed hide is rejected");
        check(InputMethods.show(current) && shows() == 2, "new window can show keyboard");
        ViewRoot.roots().remove(0);
        Field stack = AppRunner.class.getDeclaredField("sStack");
        stack.setAccessible(true);
        @SuppressWarnings("unchecked")
        java.util.ArrayList<Activity> activities = (java.util.ArrayList<Activity>) stack.get(null);
        activities.add((Activity) current.getContext());
        oldInput.run();
        check(commits == 0, "queued old text is discarded after generation changes");
        SharedMicrogWorker.deliverInput(14, 11, 2, new String[] { "14", "b", "2", "fixture" });
        check(commits == 1, "current text reaches the current window");
        InputMethods.hide(current);
        check(hides() == 2, "new window can hide keyboard");
        Field headless = AppRunner.class.getDeclaredField("sHeadless");
        headless.setAccessible(true);
        headless.setBoolean(null, true);
        Field scheduled = ViewRoot.class.getDeclaredField("sScheduled");
        scheduled.setAccessible(true);
        scheduled.setBoolean(null, true);
        window((Activity) current.getContext(), 2);
        SharedMicrogWorker.dismissUi(11, 2);
        check(((Activity) current.getContext()).isDestroyed(), "dismiss destroys the current activity");
        check(ViewRoot.roots().isEmpty(), "dismiss removes the activity and secondary windows");
        check(SharedMicrogWorker.frameChannel() == 0, "reservation is released after window removal");
        check(!SharedMicrogWorker.permitsActivityStart((Activity) current.getContext()),
            "destroyed activity cannot start another window after dismiss");
        configure(11, 3);
        View next = window(activity(), 3);
        activities.add((Activity) next.getContext());
        check(ViewRoot.sharedMicrogOwnsInput(11, 3), "new window is not blocked by previous roots");
        SharedMicrogWorker.dismissUi(11, 3);
        configure(11, 4);
        Activity broken = new Activity() {
            @Override protected void onDestroy() { throw new IllegalStateException("fixture"); }
        };
        window(broken, 4);
        activities.add(broken);
        boolean failed = false;
        try { SharedMicrogWorker.dismissUi(11, 4); }
        catch (IllegalStateException expected) { failed = true; }
        check(failed && SharedMicrogWorker.frameChannel() == 11,
            "failed activity destruction keeps the reservation");
        SharedMicrogWorker.dismissUi(11, 4);
        check(SharedMicrogWorker.frameChannel() == 11,
            "repeated dismiss cannot release a reservation after failed destruction");
        active.setBoolean(null, false);
        InputMethods.hide();
        new InputMethodManager().hideSoftInputFromWindow(null, 0);
        check(hides() == 4, "ordinary applications retain unattributed hide");
        check(InputMethods.show(old) && shows() == 3, "ordinary applications retain show");
        System.out.println("Shared microG keyboard ownership test passed");
    }
}
