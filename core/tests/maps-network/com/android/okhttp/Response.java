package com.android.okhttp;

public final class Response {
    private final int code;
    public Response(int code) { this.code = code; }
    public int code() { return code; }
}
