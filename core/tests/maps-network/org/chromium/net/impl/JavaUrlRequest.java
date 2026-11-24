package org.chromium.net.impl;

import java.io.IOException;
import java.nio.ByteBuffer;
import org.chromium.net.CronetException;
import org.chromium.net.UrlResponseInfo;

public final class JavaUrlRequest {
    private UrlResponseInfo mUrlResponseInfo;
    public final AsyncUrlRequestCallback callback = new AsyncUrlRequestCallback();
    private boolean started;

    public void start() {
        if (started) throw new IllegalStateException("already started");
        started = true;
    }
    private void processReadResult(int read, ByteBuffer buffer) throws IOException {
        if (read == -2) throw new IOException("original exception");
    }
    private void fireRedirectReceived(String url, UrlResponseInfo info) {}
    public void response(UrlResponseInfo info) { mUrlResponseInfo = info; }
    public final class AsyncUrlRequestCallback {
        private void onResponseStarted() {}
        private void onSucceeded(UrlResponseInfo info) {}
        private void onCanceled(UrlResponseInfo info) {}
        private void onFailed(UrlResponseInfo info, CronetException error) {}
    }
}
