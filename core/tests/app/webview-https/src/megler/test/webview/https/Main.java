package megler.test.webview.https;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.net.http.SslError;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.util.Log;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.ClientCertRequest;
import android.webkit.HttpAuthHandler;
import android.webkit.SslErrorHandler;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.Locale;

public final class Main extends Activity {
    private static final String TAG = "WebViewHttpsSmoke";
    private final ArrayDeque<String> events = new ArrayDeque<>();
    private TextView status;
    private EditText address;
    private WebView web;
    private volatile boolean destroyed;
    private volatile String targetUrl;
    private boolean mainError;
    private long loadId;
    private long startedAt;
    private String probeStatus = "PROBE_HTTP pending";
    private String webStatus = "WEB_HTTP success status unavailable";
    private String visualStatus = "VISUAL not confirmed";

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
        status.setPadding(dp(8), dp(4), dp(8), dp(4));
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(142)));
        LinearLayout addressBar = new LinearLayout(this);
        address = new EditText(this);
        address.setSingleLine(true);
        address.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        address.setImeOptions(EditorInfo.IME_ACTION_GO);
        address.setHint("HTTPS address");
        address.setText(RequestPolicy.DEFAULT_URL);
        address.setOnEditorActionListener((view, action, event) -> {
            if (action != EditorInfo.IME_ACTION_GO) return false;
            load();
            return true;
        });
        addressBar.addView(address, new LinearLayout.LayoutParams(0, dp(48), 1));
        Button go = new Button(this);
        go.setText("Load");
        go.setOnClickListener(v -> load());
        addressBar.addView(go);
        Button offline = new Button(this);
        offline.setText("Offline JS");
        offline.setOnClickListener(v -> startActivity(new Intent(this, OfflineJsActivity.class)));
        addressBar.addView(offline);
        root.addView(addressBar, 0, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48)));
        root.setFocusableInTouchMode(true);
        root.requestFocus();
        setContentView(root);

        try {
            web = new WebView(this);
            WebSettings settings = web.getSettings();
            settings.setJavaScriptEnabled(false);
            settings.setAllowFileAccess(false);
            settings.setAllowContentAccess(false);
            settings.setDomStorageEnabled(false);
            settings.setGeolocationEnabled(false);
            settings.setLoadsImagesAutomatically(false);
            settings.setCacheMode(WebSettings.LOAD_NO_CACHE);
            web.setWebViewClient(new WebViewClient() {
                @Override
                public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                    note("NAVIGATION_BLOCKED redirect=" + request.isRedirect());
                    return true;
                }

                @Override
                public boolean shouldOverrideUrlLoading(WebView view, String url) {
                    note("NAVIGATION_BLOCKED");
                    return true;
                }

                @Override
                public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                    boolean allow = RequestPolicy.allowInitialRequest(targetUrl, request.getUrl().toString(),
                            request.getMethod(), request.isForMainFrame(), request.isRedirect());
                    if (allow) {
                        eventFromWorker("WEB_MAIN_REQUEST");
                        return null;
                    }
                    eventFromWorker("RESOURCE_BLOCKED main=" + request.isForMainFrame());
                    return new WebResourceResponse("text/plain", "utf-8", 403, "Blocked",
                            Collections.emptyMap(), new ByteArrayInputStream(new byte[0]));
                }

                @Override
                public void onPageStarted(WebView view, String url, Bitmap icon) {
                    note("WEB_LOAD_START");
                }

                @Override
                public void onPageFinished(WebView view, String url) {
                    note("WEB_LOAD_FINISH (HTTP success status not exposed)");
                    if (!mainError && url.equals(targetUrl)) {
                        final long id = loadId;
                        view.postVisualStateCallback(id, new WebView.VisualStateCallback() {
                            @Override public void onComplete(long requestId) {
                                if (!destroyed && requestId == loadId && !mainError) {
                                    note("VISUAL_READY next draw; verify pixels on screen");
                                }
                            }
                        });
                    }
                }

                @Override
                public void onPageCommitVisible(WebView view, String url) {
                    note("VISUAL_COMMIT next draw; not a pixel check");
                }

                @Override
                public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                    if (request.isForMainFrame()) mainError = true;
                    note("WEB_LOAD_ERROR main=" + request.isForMainFrame() + " code=" + error.getErrorCode());
                }

                @Override
                public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse error) {
                    if (request.isForMainFrame()) mainError = true;
                    note("WEB_HTTP_ERROR main=" + request.isForMainFrame() + " status=" + error.getStatusCode());
                }

                @Override
                public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
                    mainError = true;
                    handler.cancel();
                    note("WEB_TLS_ERROR code=" + error.getPrimaryError() + " cancelled");
                }

                @Override
                public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String host, String realm) {
                    handler.cancel();
                    mainError = true;
                    note("WEB_HTTP_AUTH_CANCELLED");
                }

                @Override
                public void onReceivedClientCertRequest(WebView view, ClientCertRequest request) {
                    request.cancel();
                    mainError = true;
                    note("WEB_CLIENT_CERT_CANCELLED");
                }
            });
            root.addView(web, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
            note("CREATE_OK WebView");
            load();
        } catch (RuntimeException | LinkageError failure) {
            note("CREATE_FAILED " + failure.getClass().getSimpleName());
            Log.e(TAG, "WebView setup failed", failure);
        }
    }

    private void load() {
        if (web == null) { note("WEB_LOAD_UNAVAILABLE"); return; }
        final String next;
        try {
            next = RequestPolicy.normalizeAddress(address.getText().toString());
        } catch (IllegalArgumentException invalid) {
            address.setError(invalid.getMessage());
            return;
        }
        address.setError(null);
        InputMethodManager keyboard = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (keyboard != null) keyboard.hideSoftInputFromWindow(address.getWindowToken(), 0);
        web.stopLoading();
        targetUrl = next;
        mainError = false;
        loadId++;
        probeStatus = "PROBE_HTTP pending";
        webStatus = "WEB_HTTP success status unavailable";
        visualStatus = "VISUAL not confirmed";
        note("WEB_NAVIGATE selected HTTPS address");
        web.loadUrl(next);
        startStatusProbe(next, loadId);
    }

    private void startStatusProbe(String url, long id) {
        new Thread(() -> {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(url).openConnection();
                connection.setInstanceFollowRedirects(false);
                connection.setUseCaches(false);
                connection.setConnectTimeout(10000);
                connection.setReadTimeout(10000);
                connection.setRequestMethod("GET");
                int statusCode = connection.getResponseCode();
                probeEvent(id, "PROBE_HTTP_STATUS " + statusCode + " (separate request; redirects disabled)");
            } catch (IOException | SecurityException failure) {
                probeEvent(id, "PROBE_ERROR " + failure.getClass().getSimpleName());
            } finally {
                if (connection != null) connection.disconnect();
            }
        }, "https-status-probe").start();
    }

    private void probeEvent(long id, String event) {
        runOnUiThread(() -> { if (!destroyed && loadId == id) note(event); });
    }

    private void eventFromWorker(String event) {
        runOnUiThread(() -> { if (!destroyed) note(event); });
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
        note("ACTIVITY_DESTROY");
        super.onDestroy();
    }

    private void note(String event) {
        String line = String.format(Locale.ROOT, "%6d ms  %s",
                SystemClock.elapsedRealtime() - startedAt, event);
        Log.i(TAG, line);
        if (event.startsWith("PROBE_HTTP_STATUS") || event.startsWith("PROBE_ERROR")) probeStatus = event;
        if (event.startsWith("WEB_HTTP_ERROR main=true") || event.startsWith("WEB_LOAD_ERROR main=true")
                || event.startsWith("WEB_TLS_ERROR") || event.startsWith("WEB_HTTP_AUTH_CANCELLED")
                || event.startsWith("WEB_CLIENT_CERT_CANCELLED")) webStatus = event;
        if (event.startsWith("VISUAL_")) visualStatus = event;
        if (events.size() == 3) events.removeFirst();
        events.addLast(line);
        if (status != null) {
            StringBuilder text = new StringBuilder("Android WebView HTTPS smoke\n");
            text.append(probeStatus).append('\n').append(webStatus).append('\n')
                    .append(visualStatus).append('\n');
            for (String item : events) text.append(item).append('\n');
            status.setText(text);
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
