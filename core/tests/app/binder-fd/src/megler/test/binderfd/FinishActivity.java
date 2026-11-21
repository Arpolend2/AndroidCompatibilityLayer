package megler.test.binderfd;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class FinishActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        finish();
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        Log.i("BinderGuestTest", "PASS headless activity destroyed");
    }
}
