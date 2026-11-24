package org.chromium.net;

public class UrlResponseInfo {
    private final int code;
    public UrlResponseInfo(int code) { this.code = code; }
    public int getHttpStatusCode() { return code; }
}
