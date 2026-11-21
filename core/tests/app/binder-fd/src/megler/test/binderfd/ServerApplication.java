package megler.test.binderfd;

import android.app.Application;

public class ServerApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        Main.startHeadless(this);
    }
}
