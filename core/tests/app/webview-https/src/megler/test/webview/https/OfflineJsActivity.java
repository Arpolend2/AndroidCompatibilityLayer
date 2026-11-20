package megler.test.webview.https;

import android.app.Activity;
import android.os.Bundle;
import android.util.Base64;
import android.util.Log;
import android.view.ViewGroup;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;

public final class OfflineJsActivity extends Activity {
    private static final String TAG = "WebViewOfflineJsSmoke";
    private static final String PAGE = "<!doctype html><meta charset=utf-8>"
            + "<meta name=viewport content='width=device-width, initial-scale=1'>"
            + "<meta http-equiv=Content-Security-Policy content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'\">"
            + "<title>Offline JS smoke</title>"
            + "<style>body{font:18px sans-serif;margin:.8em}h1{font-size:1.25em;margin:0 0 .4em}"
            + "p{margin:.4em 0;overflow-wrap:break-word}input,button{font:inherit;padding:.4em}"
            + "input{display:block;width:100%;max-width:30em;box-sizing:border-box}</style>"
            + "<h1>Offline JavaScript</h1>"
            + "<label for=sample>Type sample text (no personal data)</label>"
            + "<input id=sample maxlength=80 autocomplete=off spellcheck=false disabled>"
            + "<p><output id=echo for=sample aria-live=polite>No text yet</output></p>"
            + "<p id=status>Waiting for JavaScript</p>"
            + "<button id=tap disabled>Tap to test</button>"
            + "<script>(function(){"
            + "var status=document.getElementById('status'),button=document.getElementById('tap'),count=0;"
            + "var sample=document.getElementById('sample'),echo=document.getElementById('echo');"
            + "sample.addEventListener('input',function(){"
            + "echo.textContent=sample.value?'Entered: '+sample.value:'No text yet';"
            + "});"
            + "button.addEventListener('click',function(){"
            + "count++;status.textContent='Tap received: '+count;document.title='JS_TAP '+count;"
            + "});"
            + "setTimeout(function(){status.textContent='JavaScript ready';button.disabled=false;sample.disabled=false;"
            + "document.title='JS_READY';},0);"
            + "})();</script>";

    private WebView web;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            web = new WebView(this);
            WebSettings settings = web.getSettings();
            settings.setBlockNetworkLoads(true);
            settings.setAllowFileAccess(false);
            settings.setAllowContentAccess(false);
            settings.setJavaScriptEnabled(true);
            web.setWebViewClient(new WebViewClient() {
                @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                    Log.w(TAG, "NAVIGATION_BLOCKED");
                    return true;
                }
            });
            web.setWebChromeClient(new WebChromeClient() {
                @Override public void onReceivedTitle(WebView view, String title) {
                    if ("JS_READY".equals(title) || (title != null && title.matches("JS_TAP [0-9]+"))) {
                        Log.i(TAG, title);
                    }
                }
            });
            setContentView(web);
            web.loadData(Base64.encodeToString(PAGE.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP),
                    "text/html", "base64");
        } catch (RuntimeException | LinkageError failure) {
            Log.e(TAG, "CREATE_FAILED Offline JavaScript WebView", failure);
            TextView error = new TextView(this);
            error.setText("Offline WebView unavailable: " + failure.getClass().getSimpleName());
            setContentView(error);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) {
            web.resumeTimers();
            web.onResume();
        }
    }

    @Override
    protected void onPause() {
        if (web != null) web.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.stopLoading();
            if (web.getParent() instanceof ViewGroup) ((ViewGroup) web.getParent()).removeView(web);
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
