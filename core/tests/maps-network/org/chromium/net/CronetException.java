package org.chromium.net;

public class CronetException extends Exception {
    public CronetException(Throwable cause) { super("sensitive exception text", cause); }
}
