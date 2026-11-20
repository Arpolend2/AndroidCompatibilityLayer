package megler.test.webview.loopback;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.security.NetworkSecurityPolicy;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.Locale;

public final class Main extends Activity {
    private static final String TAG = "WebViewLoopbackSmoke";
    private final ArrayDeque<String> events = new ArrayDeque<>();
    private TextView status;
    private WebView web;
    private LoopbackServer server;
    private String baseUrl;
    private int serverPort;
    private boolean destroyed;
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
        status.setTextSize(13);
        status.setGravity(Gravity.CENTER_VERTICAL);
        status.setPadding(dp(8), dp(4), dp(8), dp(4));
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(88)));
        root.addView(controls(), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52)));
        setContentView(root);

        try {
            web = new WebView(this);
            web.getSettings().setJavaScriptEnabled(false);
            web.setWebViewClient(new WebViewClient() {
                @Override
                public void onPageStarted(WebView view, String url, Bitmap icon) {
                    note("LOAD_START " + url);
                }

                @Override
                public void onPageFinished(WebView view, String url) {
                    note("LOAD_FINISH " + url + " title=" + view.getTitle());
                }

                @Override
                public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                    return rejectExternal(request.getUrl().toString());
                }

                @Override
                public boolean shouldOverrideUrlLoading(WebView view, String url) {
                    return rejectExternal(url);
                }

                @Override
                public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                    note("LOAD_ERROR " + request.getUrl() + " main=" + request.isForMainFrame()
                            + " code=" + error.getErrorCode() + " " + error.getDescription());
                }

                @Override
                public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse error) {
                    note("HTTP_ERROR " + request.getUrl() + " status=" + error.getStatusCode());
                }
            });
            root.addView(web, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
            note("CREATE_OK WebView");
            NetworkSecurityPolicy policy = NetworkSecurityPolicy.getInstance();
            note("POLICY loopback=" + policy.isCleartextTrafficPermitted("127.0.0.1")
                    + " external=" + policy.isCleartextTrafficPermitted("example.invalid"));
            server = new LoopbackServer(new LoopbackServer.Listener() {
                @Override
                public void ready(int port) {
                    runOnUiThread(() -> {
                        if (destroyed) return;
                        serverPort = port;
                        baseUrl = "http://127.0.0.1:" + port;
                        note("SERVER_READY " + baseUrl);
                        load("/page1");
                    });
                }

                @Override
                public void event(String message) {
                    runOnUiThread(() -> { if (!destroyed) note(message); });
                }
            });
            server.start();
        } catch (RuntimeException | LinkageError failure) {
            note("CREATE_FAILED " + failure);
            Log.e(TAG, "WebView setup failed", failure);
        }
    }

    private View controls() {
        LinearLayout row = new LinearLayout(this);
        addButton(row, "Page 1", () -> load("/page1"));
        addButton(row, "Page 2", () -> load("/page2"));
        addButton(row, "Back", () -> navigate(true));
        addButton(row, "Forward", () -> navigate(false));
        addButton(row, "Reload", () -> {
            if (web != null) { note("RELOAD"); web.reload(); }
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

    private void load(String path) {
        if (web == null || baseUrl == null) {
            note("NAVIGATE_UNAVAILABLE server or WebView not ready");
            return;
        }
        note("NAVIGATE " + baseUrl + path);
        web.loadUrl(baseUrl + path);
    }

    private boolean rejectExternal(String url) {
        Uri uri = Uri.parse(url);
        boolean allowed = "http".equals(uri.getScheme()) && "127.0.0.1".equals(uri.getHost())
                && uri.getPort() == serverPort && uri.getUserInfo() == null;
        note((allowed ? "NAVIGATE_LINK " : "NAVIGATE_BLOCKED ") + url);
        return !allowed;
    }

    private void navigate(boolean back) {
        if (web == null) { note("HISTORY_UNAVAILABLE"); return; }
        boolean available = back ? web.canGoBack() : web.canGoForward();
        note((back ? "BACK" : "FORWARD") + (available ? "_OK" : "_EMPTY"));
        if (available) {
            if (back) web.goBack(); else web.goForward();
        }
    }

    @Override
    protected void onPause() {
        if (web != null) { web.onPause(); web.pauseTimers(); }
        note("ACTIVITY_PAUSE");
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) { web.resumeTimers(); web.onResume(); }
        note("ACTIVITY_RESUME");
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        if (web != null) {
            web.stopLoading();
            if (web.getParent() instanceof ViewGroup) ((ViewGroup) web.getParent()).removeView(web);
            web.destroy();
            web = null;
        }
        if (server != null) {
            try { server.close(); }
            catch (IOException failure) { note("SERVER_CLOSE_ERROR " + failure); }
        }
        note("ACTIVITY_DESTROY");
        super.onDestroy();
    }

    private void note(String event) {
        String line = String.format(Locale.ROOT, "%6d ms  %s",
                SystemClock.elapsedRealtime() - startedAt, event);
        Log.i(TAG, line);
        if (events.size() == 4) events.removeFirst();
        events.addLast(line);
        if (status != null) {
            StringBuilder text = new StringBuilder("Android WebView loopback smoke\n");
            for (String item : events) text.append(item).append('\n');
            status.setText(text);
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
