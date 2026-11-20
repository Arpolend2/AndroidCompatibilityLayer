package megler.test.webview.smoke;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayDeque;
import java.util.Locale;

/** Small device smoke test for Android's normal System WebView path. */
public final class Main extends Activity {
    private static final String TAG = "AndroidWebViewSmoke";
    private static final String HOME_URL = "file:///android_asset/index.html";
    private static final String SECOND_URL = "file:///android_asset/second.html";

    private final ArrayDeque<String> events = new ArrayDeque<>();
    private TextView status;
    private WebView web;
    private long startedAt;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        startedAt = SystemClock.elapsedRealtime();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);

        status = new TextView(this);
        status.setTextColor(Color.BLACK);
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER_VERTICAL);
        status.setPadding(dp(8), dp(4), dp(8), dp(4));
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(80)));
        root.addView(controls(), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52)));

        try {
            web = new WebView(this);
            web.setWebViewClient(new WebViewClient() {
                @Override
                public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) {
                    note("LOAD_START " + url);
                }

                @Override
                public void onPageFinished(WebView view, String url) {
                    note("LOAD_FINISH " + url + " title=" + view.getTitle());
                }

                @Override
                public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                    if (request.isForMainFrame()) {
                        note("LOAD_ERROR " + request.getUrl() + " code=" + error.getErrorCode()
                                + " " + error.getDescription());
                    }
                }
            });
            web.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                int width = right - left, height = bottom - top;
                if (width > 0 && height > 0 && (width != oldRight - oldLeft || height != oldBottom - oldTop)) {
                    note("VIEWPORT " + width + "x" + height);
                }
            });
            root.addView(web, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
            setContentView(root);
            note("CREATE_OK WebView instance created; opening local asset page");
            web.loadUrl(HOME_URL);
        } catch (RuntimeException | LinkageError failure) {
            setContentView(root);
            note("CREATE_FAILED " + describe(failure));
            Log.e(TAG, "WebView creation failed", failure);
        }
    }

    private View controls() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        addButton(row, "Home", () -> load(HOME_URL));
        addButton(row, "Page 2", () -> load(SECOND_URL));
        addButton(row, "Back", () -> navigate("BACK", true));
        addButton(row, "Forward", () -> navigate("FORWARD", false));
        addButton(row, "Reload", () -> {
            if (web != null) {
                note("RELOAD");
                web.reload();
            }
        });
        return row;
    }

    private void addButton(LinearLayout row, String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setTextSize(12);
        button.setOnClickListener(view -> action.run());
        row.addView(button, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.MATCH_PARENT, 1));
    }

    private void load(String url) {
        if (web == null) {
            note("NAVIGATE_UNAVAILABLE no WebView instance");
            return;
        }
        note("NAVIGATE " + url);
        web.loadUrl(url);
    }

    private void navigate(String direction, boolean back) {
        if (web == null) {
            note(direction + "_UNAVAILABLE no WebView instance");
            return;
        }
        boolean available = back ? web.canGoBack() : web.canGoForward();
        note(direction + (available ? "_OK" : "_EMPTY"));
        if (available) {
            if (back) web.goBack(); else web.goForward();
        }
    }

    @Override
    protected void onPause() {
        if (web != null) {
            web.onPause();
            web.pauseTimers();
        }
        note("ACTIVITY_PAUSE");
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) {
            web.resumeTimers();
            web.onResume();
        }
        note("ACTIVITY_RESUME");
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.stopLoading();
            web.onPause();
            if (web.getParent() instanceof android.view.ViewGroup) {
                ((android.view.ViewGroup) web.getParent()).removeView(web);
            }
            web.destroy();
            web = null;
        }
        note("ACTIVITY_DESTROY");
        super.onDestroy();
    }

    private void note(String event) {
        long elapsed = SystemClock.elapsedRealtime() - startedAt;
        String line = String.format(Locale.ROOT, "%6d ms  %s", elapsed, event);
        Log.i(TAG, line);
        if (events.size() == 3) events.removeFirst();
        events.addLast(line);
        if (status != null) {
            StringBuilder text = new StringBuilder("Android System WebView smoke test\n");
            for (String item : events) text.append(item).append('\n');
            status.setText(text);
        }
    }

    private static String describe(Throwable error) {
        Throwable cause = error;
        while (cause.getCause() != null && cause.getCause() != cause) cause = cause.getCause();
        return error.getClass().getSimpleName() + ": " + error.getMessage()
                + (cause == error ? "" : " | cause: " + cause.getClass().getSimpleName() + ": " + cause.getMessage());
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
