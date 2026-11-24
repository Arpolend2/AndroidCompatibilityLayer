package com.android.okhttp.internal.http;

import com.android.okhttp.Response;

public final class HttpEngine {
    private final Response response;
    public HttpEngine(int code) { response = new Response(code); }
    public Response getResponse() { return response; }
}
