package megler.test.binderfd;

import android.app.Application;
import android.content.Intent;

public class ActivityServerApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        startActivity(new Intent(this, FinishActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        Main.startHeadless(this);
    }
}
