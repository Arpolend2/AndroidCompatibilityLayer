package com.android.okhttp.internal.huc;

import com.android.okhttp.internal.http.HttpEngine;
import java.io.IOException;
import java.net.UnknownHostException;

public final class HttpURLConnectionImpl {
    public boolean fail;
    public IOException ioFailure;
    public RuntimeException runtimeFailure;
    public int code = 200;
    public void connect() throws IOException {
        if (fail) throw new UnknownHostException("secret.example");
    }
    private HttpEngine getResponse() throws IOException {
        if (fail) throw new UnknownHostException("secret.example");
        if (ioFailure != null) throw ioFailure;
        if (runtimeFailure != null) throw runtimeFailure;
        return new HttpEngine(code);
    }
    public void disconnect() {}
}
