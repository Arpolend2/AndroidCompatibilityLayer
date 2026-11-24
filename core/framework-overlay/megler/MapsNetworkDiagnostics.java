package megler;

import java.io.EOFException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.ConnectException;
import java.net.NoRouteToHostException;
import java.net.ProtocolException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.security.cert.CertificateException;
import java.util.IdentityHashMap;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLPeerUnverifiedException;

/** Metadata only, for the Java fallback Cronet used by Maps. */
final class MapsNetworkDiagnostics {
    private static final String REQUEST = "org.chromium.net.impl.JavaUrlRequest";
    private static final int MAX_ACTIVE = 2048, MAX_EVENTS = 10000;
    private static final IdentityHashMap<Object, Record> active = new IdentityHashMap<>();
    private static long nextId;
    private static int events;
    private static boolean overflow;

    private static final class Record {
        final long id;
        long bytes;
        int status;
        Record(long id) { this.id = id; }
    }

    private final String kind;
    private Method original;
    private Field owner, response;

    private MapsNetworkDiagnostics(String kind) { this.kind = kind; }

    static void install(ClassLoader loader) {
        if (!"com.google.android.apps.maps".equals(Native.packageName())) return;
        MapsSystemHttpDiagnostics.install();
        String stage = "request_class";
        try {
            Class<?> request = Class.forName(REQUEST, false, loader);
            stage = "callback_class";
            Class<?> callback = Class.forName(REQUEST + "$AsyncUrlRequestCallback", false, loader);
            stage = "owner_field";
            Field owner = callback.getDeclaredField("this$0");
            stage = "response_field";
            Field response = request.getDeclaredField("mUrlResponseInfo");
            owner.setAccessible(true);
            response.setAccessible(true);
            stage = "response_class";
            Class<?> info = Class.forName("org.chromium.net.UrlResponseInfo", false, loader);
            stage = "error_class";
            Class<?> error = Class.forName("org.chromium.net.CronetException", false, loader);
            stage = "methods";
            Method[] methods = {
                request.getDeclaredMethod("start"),
                request.getDeclaredMethod("processReadResult", int.class, ByteBuffer.class),
                request.getDeclaredMethod("fireRedirectReceived", String.class, info),
                callback.getDeclaredMethod("onResponseStarted"),
                callback.getDeclaredMethod("onSucceeded", info),
                callback.getDeclaredMethod("onCanceled", info),
                callback.getDeclaredMethod("onFailed", info, error)
            };
            String[] kinds = { "start", "read", "redirect", "response", "success", "cancel", "failure" };
            for (int i = 0; i < methods.length; i++) {
                stage = "hook_" + kinds[i];
                hook(methods[i], kinds[i], i >= 3 ? owner : null, i == 3 ? response : null);
            }
            line("ready engine=java-cronet scope=urlrequest");
        } catch (ReflectiveOperationException | LinkageError | RuntimeException e) {
            line("unavailable engine=java-cronet stage=" + stage + " coverage=partial");
        }
    }

    private static void hook(Method method, String kind, Field owner, Field response)
            throws ReflectiveOperationException {
        MapsNetworkDiagnostics h = new MapsNetworkDiagnostics(kind);
        h.owner = owner;
        h.response = response;
        h.original = Hooks.hook0(h, method,
                MapsNetworkDiagnostics.class.getDeclaredMethod("callback", Object[].class));
        if (h.original == null) throw new IllegalStateException("hook failed");
        h.original.setAccessible(true);
    }

    Object callback(Object[] args) throws Throwable {
        boolean started = false;
        try {
            Object request = owner == null ? args[0] : owner.get(args[0]);
            if ("start".equals(kind)) started = start(request);
            else if ("read".equals(kind)) read(request, (Integer) args[1]);
            else if ("redirect".equals(kind)) status(request, args[2], "redirect");
            else if ("response".equals(kind)) status(request, response.get(request), "response");
            else if ("success".equals(kind) || "cancel".equals(kind))
                finish(request, kind, args[1], null);
            else if ("failure".equals(kind)) finish(request, kind, args[1], (Throwable) args[2]);
        } catch (ReflectiveOperationException | RuntimeException e) {
            line("instrumentation_error");
        }
        Object[] rest = new Object[args.length - 1];
        System.arraycopy(args, 1, rest, 0, rest.length);
        try {
            return original.invoke(args[0], rest);
        } catch (InvocationTargetException e) {
            if (started) discard(args[0]);
            throw e.getCause();
        }
    }

    private static synchronized boolean start(Object request) {
        if (active.containsKey(request)) return false;
        if (active.size() >= MAX_ACTIVE) {
            line("capacity_exceeded");
            return false;
        }
        Record r = new Record(++nextId);
        active.put(request, r);
        line("engine=java-cronet id=" + r.id + " event=start");
        return true;
    }

    private static synchronized void discard(Object request) {
        Record r = active.remove(request);
        if (r != null) line("engine=java-cronet id=" + r.id + " event=start_failed");
    }

    private static synchronized void read(Object request, int count) {
        Record r = active.get(request);
        if (r != null && count > 0) r.bytes = r.bytes > Long.MAX_VALUE - count ? Long.MAX_VALUE : r.bytes + count;
    }

    private static int statusCode(Object info) throws ReflectiveOperationException {
        if (info == null) return 0;
        Method m = info.getClass().getMethod("getHttpStatusCode");
        m.setAccessible(true);
        int code = ((Integer) m.invoke(info)).intValue();
        return code >= 100 && code <= 599 ? code : 0;
    }

    private static synchronized void status(Object request, Object info, String event)
            throws ReflectiveOperationException {
        Record r = active.get(request);
        if (r == null) return;
        r.status = statusCode(info);
        line("engine=java-cronet id=" + r.id + " event=" + event + " http=" + r.status);
    }

    private static synchronized void finish(Object request, String event, Object info, Throwable error)
            throws ReflectiveOperationException {
        Record r = active.remove(request);
        if (r == null) return;
        int status = statusCode(info);
        if (status != 0) r.status = status;
        line("engine=java-cronet id=" + r.id + " event=" + event + " http=" + r.status +
                " body_bytes_read=" + r.bytes + " error=" + errorKind(error));
    }

    static String errorKind(Throwable t) {
        if (t == null) return "none";
        boolean tls = false;
        for (int i = 0; t != null && i < 16; i++, t = t.getCause()) {
            if (t instanceof CertificateException || t instanceof SSLPeerUnverifiedException) return "certificate";
            if (t instanceof SSLException) tls = true;
            if (!tls && t instanceof UnknownHostException) return "dns";
            if (!tls && t instanceof SocketTimeoutException) return "timeout";
            if (!tls && t instanceof NoRouteToHostException) return "unreachable";
            if (!tls && t instanceof ConnectException) return "connect";
            if (!tls && t instanceof ProtocolException) return "protocol";
            if (!tls && t instanceof EOFException) return "eof";
        }
        return tls ? "tls" : "other";
    }

    private static synchronized void line(String text) {
        if (events++ < MAX_EVENTS) android.util.Log.i("Megler", "maps-net: " + text);
        else if (!overflow) {
            overflow = true;
            android.util.Log.w("Megler", "maps-net: event_limit_reached");
        }
    }
}
