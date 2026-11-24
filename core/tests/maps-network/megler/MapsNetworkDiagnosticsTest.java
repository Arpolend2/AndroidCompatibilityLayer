package megler;

import android.util.Log;
import java.io.IOException;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.security.cert.CertificateException;
import javax.net.ssl.SSLHandshakeException;
import org.chromium.net.CronetException;
import org.chromium.net.UrlResponseInfo;
import org.chromium.net.impl.JavaUrlRequest;
import com.android.okhttp.internal.huc.HttpURLConnectionImpl;

public final class MapsNetworkDiagnosticsTest {
    private static final String REQUEST = "org.chromium.net.impl.JavaUrlRequest";
    private static final String CALLBACK = REQUEST + "$AsyncUrlRequestCallback";
    private static final class CheckedA extends Exception {}
    private static final class CheckedB extends Exception {}
    private static final class CheckedBroken extends Exception {
        @Override public StackTraceElement[] getStackTrace() { throw new SecurityException("secret"); }
    }

    private static void has(String text, String value) {
        if (!text.contains(value)) throw new AssertionError("missing: " + value + "\n" + text);
    }

    private static void systemFailure(String system, Throwable cause) throws Throwable {
        HttpURLConnectionImpl connection = new HttpURLConnectionImpl();
        connection.ioFailure = new IOException("secret", cause);
        try {
            Hooks.call(system, "getResponse", connection);
            throw new AssertionError("I/O error lost");
        } catch (IOException expected) {
            if (expected != connection.ioFailure) throw new AssertionError("I/O error changed");
        }
    }

    public static void main(String[] args) throws Throwable {
        ClassLoader loader = MapsNetworkDiagnosticsTest.class.getClassLoader();
        if (args.length != 0 && args[0].equals("hook-failure"))
            Hooks.rejected = "com.android.okhttp.internal.huc.HttpURLConnectionImpl#getResponse";
        if (args.length != 0 && args[0].startsWith("missing-")) {
            final String denied = args[0].equals("missing-class") ? REQUEST : CALLBACK;
            loader = new ClassLoader(loader) {
                @Override public Class<?> loadClass(String name) throws ClassNotFoundException {
                    if (name.equals(denied)) throw new ClassNotFoundException("secret");
                    return super.loadClass(name);
                }
            };
        }
        MapsNetworkDiagnostics.install(loader);
        HttpURLConnectionImpl http = new HttpURLConnectionImpl(), failed = new HttpURLConnectionImpl();
        HttpURLConnectionImpl implicit = new HttpURLConnectionImpl(), closed = new HttpURLConnectionImpl();
        HttpURLConnectionImpl io = new HttpURLConnectionImpl(), runtime = new HttpURLConnectionImpl();
        HttpURLConnectionImpl cyclic = new HttpURLConnectionImpl();
        HttpURLConnectionImpl errno = new HttpURLConnectionImpl(), gai = new HttpURLConnectionImpl();
        HttpURLConnectionImpl algorithm = new HttpURLConnectionImpl(), plain = new HttpURLConnectionImpl();
        String system = "com.android.okhttp.internal.huc.HttpURLConnectionImpl";
        if (args.length != 0 && args[0].equals("hook-failure")) {
            Hooks.call(system, "connect", http);
            has(Log.output.toString(), "unavailable engine=system-http stage=hook_response coverage=partial");
            if (Log.output.toString().contains("ready engine=system-http"))
                throw new AssertionError("partial installation reported as ready");
            System.out.println("Maps network hook failure test passed");
            return;
        }
        Hooks.call(system, "connect", http);
        http.code = 403;
        Object engine = Hooks.call(system, "getResponse", http);
        if (engine == null) throw new AssertionError("lost response");
        Hooks.call(system, "getResponse", http);
        Hooks.call(system, "disconnect", http);
        failed.fail = true;
        try {
            Hooks.call(system, "connect", failed);
            throw new AssertionError("connection error lost");
        } catch (UnknownHostException expected) {}
        Hooks.call(system, "disconnect", failed);
        implicit.code = 302;
        Hooks.call(system, "getResponse", implicit);
        Hooks.call(system, "disconnect", implicit);
        Hooks.call(system, "connect", closed);
        Hooks.call(system, "disconnect", closed);
        io.ioFailure = new IOException("secret", new IllegalStateException("secret"));
        try {
            Hooks.call(system, "getResponse", io);
            throw new AssertionError("I/O error lost");
        } catch (IOException expected) {
            if (expected != io.ioFailure) throw new AssertionError("I/O error changed");
        }
        runtime.runtimeFailure = new NullPointerException("secret");
        try {
            Hooks.call(system, "getResponse", runtime);
            throw new AssertionError("runtime error lost");
        } catch (NullPointerException expected) {
            if (expected != runtime.runtimeFailure) throw new AssertionError("runtime error changed");
        }
        cyclic.runtimeFailure = new RuntimeException("secret") {
            @Override public synchronized Throwable getCause() { return this; }
        };
        try {
            Hooks.call(system, "getResponse", cyclic);
            throw new AssertionError("cyclic error lost");
        } catch (RuntimeException expected) {
            if (expected != cyclic.runtimeFailure) throw new AssertionError("cyclic error changed");
        }
        errno.ioFailure = new IOException("secret", new android.system.ErrnoException(13));
        try {
            Hooks.call(system, "getResponse", errno);
            throw new AssertionError("errno error lost");
        } catch (IOException expected) {
            if (expected != errno.ioFailure) throw new AssertionError("errno error changed");
        }
        gai.runtimeFailure = new android.system.GaiException(-2);
        try {
            Hooks.call(system, "getResponse", gai);
            throw new AssertionError("gai error lost");
        } catch (android.system.GaiException expected) {
            if (expected != gai.runtimeFailure) throw new AssertionError("gai error changed");
        }
        java.security.NoSuchAlgorithmException cause = new java.security.NoSuchAlgorithmException("secret");
        cause.setStackTrace(new StackTraceElement[] {
                new StackTraceElement("com.android.okhttp.secret", "secret", "secret", 42)
        });
        algorithm.ioFailure = new IOException("secret", cause);
        try {
            Hooks.call(system, "getResponse", algorithm);
            throw new AssertionError("algorithm error lost");
        } catch (IOException expected) {
            if (expected != algorithm.ioFailure) throw new AssertionError("algorithm error changed");
        }
        plain.ioFailure = new IOException("secret", new Exception("secret"));
        try {
            Hooks.call(system, "getResponse", plain);
            throw new AssertionError("checked error lost");
        } catch (IOException expected) {
            if (expected != plain.ioFailure) throw new AssertionError("checked error changed");
        }
        String systemOut = Log.output.toString();
        has(systemOut, "ready engine=system-http scope=connection bytes=unavailable");
        has(systemOut, "engine=system-http id=1 event=response http=403 body_bytes_read=unavailable");
        has(systemOut, "engine=system-http id=2 event=failure phase=connect error=dns cause_types=dns os_errors=none body_bytes_read=unavailable");
        has(systemOut, "engine=system-http id=3 event=response http=302 body_bytes_read=unavailable");
        has(systemOut, "engine=system-http id=4 event=disconnect");
        has(systemOut, "engine=system-http id=5 event=failure phase=response error=other cause_types=io,illegal_state os_errors=none body_bytes_read=unavailable");
        has(systemOut, "engine=system-http id=6 event=failure phase=response error=other cause_types=null_pointer os_errors=none body_bytes_read=unavailable");
        has(systemOut, "cause_types=runtime,runtime,runtime,runtime,runtime,runtime,runtime,runtime,truncated");
        has(systemOut, "cause_types=io,errno os_errors=errno_13");
        has(systemOut, "cause_types=gai os_errors=gai_-2");
        has(systemOut, "cause_types=io,algorithm os_errors=none body_bytes_read=unavailable cause_origin=system_http");
        has(systemOut, "cause_types=io,exception os_errors=none");
        if (systemOut.indexOf("id=1 event=response") != systemOut.lastIndexOf("id=1 event=response"))
            throw new AssertionError("repeated response event");
        if (systemOut.contains("secret")) throw new AssertionError("sensitive data logged");
        if (java.security.Security.getProvider("GmsCore_OpenSSL") != null)
            throw new AssertionError("test requires no pre-existing microG provider");
        java.security.Provider provider = new java.security.Provider("GmsCore_OpenSSL", 1.0, "test") {};
        java.security.Security.addProvider(provider);
        try {
            CheckedA first = new CheckedA();
            first.setStackTrace(new StackTraceElement[] {
                    new StackTraceElement("com.google.android.gms.security.ProviderInstaller", "secret", "secret", 42)
            });
            systemFailure(system, first);
            java.security.Security.removeProvider("GmsCore_OpenSSL");
            systemFailure(system, new CheckedA());
            systemFailure(system, new CheckedB());
            systemFailure(system, new com.android.okhttp.internal.http.RouteException());
            systemFailure(system, new CheckedBroken());
            systemFailure(system, new Exception() {});
            systemFailure(system, new Exception() {});
            systemFailure(system, new Exception() {});
            systemFailure(system, new Exception() {});
            systemFailure(system, new Exception() {});
            systemFailure(system, new Exception() {});
            for (int i = 10; i < 17; i++) systemFailure(system, new CheckedA());
        } finally {
            java.security.Security.removeProvider("GmsCore_OpenSSL");
        }
        systemOut = Log.output.toString();
        has(systemOut, "checked_sample=recorded checked_group=1 checked_scope=other provider_frame=seen gms_provider=registered");
        has(systemOut, "checked_sample=recorded checked_group=1 checked_scope=other provider_frame=not_seen gms_provider=absent");
        has(systemOut, "checked_sample=recorded checked_group=2 checked_scope=other provider_frame=not_seen gms_provider=absent");
        has(systemOut, "checked_sample=recorded checked_group=3 checked_scope=system_http provider_frame=not_seen gms_provider=absent");
        has(systemOut, "checked_sample=recorded checked_group=overflow");
        has(systemOut, "checked_sample=unavailable");
        has(systemOut, "checked_sample=limit_reached");
        if (systemOut.contains("secret") || systemOut.contains("CheckedA") || systemOut.contains("CheckedB"))
            throw new AssertionError("sensitive details logged");
        if (args.length != 0) {
            has(systemOut, "unavailable engine=java-cronet stage=" +
                    (args[0].equals("missing-class") ? "request_class" : "callback_class"));
            System.out.println("Maps network missing-class fallback test passed");
            return;
        }
        JavaUrlRequest a = new JavaUrlRequest(), b = new JavaUrlRequest(), c = new JavaUrlRequest();
        JavaUrlRequest d = new JavaUrlRequest(), e = new JavaUrlRequest();
        Hooks.call(REQUEST, "start", a);
        Hooks.call(REQUEST, "start", b);
        try {
            Hooks.call(REQUEST, "start", a);
            throw new AssertionError("duplicate start accepted");
        } catch (IllegalStateException expected) {}
        Hooks.call(REQUEST, "fireRedirectReceived", a, "https://secret.example/?token=secret", new UrlResponseInfo(302));
        a.response(new UrlResponseInfo(200));
        Hooks.call(CALLBACK, "onResponseStarted", a.callback);
        Hooks.call(REQUEST, "processReadResult", a, 17, ByteBuffer.allocate(32));
        Hooks.call(REQUEST, "processReadResult", a, 3, ByteBuffer.allocate(32));
        Hooks.call(CALLBACK, "onFailed", b.callback, null, new CronetException(new UnknownHostException("secret")));
        Hooks.call(CALLBACK, "onSucceeded", a.callback, new UrlResponseInfo(200));
        Hooks.call(REQUEST, "start", c);
        SSLHandshakeException tls = new SSLHandshakeException("secret");
        tls.initCause(new CertificateException("secret"));
        Hooks.call(CALLBACK, "onFailed", c.callback, null, new CronetException(tls));
        Hooks.call(REQUEST, "start", d);
        Hooks.call(CALLBACK, "onCanceled", d.callback, null);
        Hooks.call(REQUEST, "start", e);
        Hooks.call(CALLBACK, "onFailed", e.callback, null, new CronetException(new IOException("secret")));
        try {
            Hooks.call(REQUEST, "processReadResult", a, -2, ByteBuffer.allocate(1));
            throw new AssertionError("original exception was lost");
        } catch (IOException expected) {
            has(expected.getMessage(), "original exception");
        }
        String out = Log.output.toString();
        has(out, "ready engine=java-cronet");
        has(out, "id=1 event=redirect http=302");
        has(out, "id=1 event=response http=200");
        has(out, "id=1 event=success http=200 body_bytes_read=20 error=none");
        has(out, "id=2 event=failure http=0 body_bytes_read=0 error=dns");
        has(out, "id=3 event=failure http=0 body_bytes_read=0 error=certificate");
        has(out, "id=4 event=cancel http=0 body_bytes_read=0 error=none");
        has(out, "id=5 event=failure http=0 body_bytes_read=0 error=other");
        if (out.contains("secret") || out.contains("http:") || out.contains("https:"))
            throw new AssertionError("sensitive data logged");
        System.out.println("Maps network metadata tests passed");
    }
}
