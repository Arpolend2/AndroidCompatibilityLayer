package megler;

import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.ConnectException;
import java.net.NoRouteToHostException;
import java.net.ProtocolException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.security.GeneralSecurityException;
import java.security.InvalidKeyException;
import java.security.KeyManagementException;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.Security;
import java.security.cert.CertificateException;
import java.util.IdentityHashMap;
import java.util.WeakHashMap;
import java.util.concurrent.ExecutionException;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLPeerUnverifiedException;

/** Metadata for the Android libcore HTTP implementation, independent of Cronet's package. */
final class MapsSystemHttpDiagnostics {
    private static final String HTTP = "com.android.okhttp.internal.huc.HttpURLConnectionImpl";
    private static final String ENGINE = "com.android.okhttp.internal.http.HttpEngine";
    private static final String RESPONSE = "com.android.okhttp.Response";
    private static final int MAX_ACTIVE = 2048, MAX_EVENTS = 10000;
    private static final int MAX_CHECKED_SAMPLES = 16, MAX_CHECKED_GROUPS = 8;
    private static final WeakHashMap<Object, Record> active = new WeakHashMap<>();
    private static final IdentityHashMap<Class<?>, Integer> checkedGroups = new IdentityHashMap<>();
    private static long nextId;
    private static int events;
    private static int originSamples;
    private static int checkedSamples;
    private static boolean overflow;
    private static Method engineResponse, responseCode;

    private static final class Record {
        final long id;
        boolean reported;
        Record(long id) { this.id = id; }
    }

    private final String kind;
    private Method original;

    private MapsSystemHttpDiagnostics(String kind) { this.kind = kind; }

    static void install() {
        String stage = "classes";
        try {
            Class<?> http = Class.forName(HTTP);
            Class<?> engine = Class.forName(ENGINE);
            Class<?> response = Class.forName(RESPONSE);
            stage = "methods";
            Method connect = http.getDeclaredMethod("connect");
            Method getResponse = http.getDeclaredMethod("getResponse");
            Method disconnect = http.getDeclaredMethod("disconnect");
            engineResponse = engine.getDeclaredMethod("getResponse");
            responseCode = response.getDeclaredMethod("code");
            engineResponse.setAccessible(true);
            responseCode.setAccessible(true);
            stage = "hook_connect";
            hook(connect, "connect");
            stage = "hook_response";
            hook(getResponse, "response");
            stage = "hook_disconnect";
            hook(disconnect, "disconnect");
            line("ready engine=system-http scope=connection bytes=unavailable");
        } catch (ReflectiveOperationException | LinkageError | RuntimeException e) {
            line("unavailable engine=system-http stage=" + stage + " coverage=partial");
        }
    }

    private static void hook(Method method, String kind) throws ReflectiveOperationException {
        MapsSystemHttpDiagnostics h = new MapsSystemHttpDiagnostics(kind);
        h.original = Hooks.hook0(h, method,
                MapsSystemHttpDiagnostics.class.getDeclaredMethod("callback", Object[].class));
        if (h.original == null) throw new IllegalStateException("hook failed");
        h.original.setAccessible(true);
    }

    Object callback(Object[] args) throws Throwable {
        Object connection = args[0];
        if (!"disconnect".equals(kind)) start(connection);
        try {
            Object result = original.invoke(connection, new Object[0]);
            if ("response".equals(kind)) {
                try {
                    Object response = engineResponse.invoke(result);
                    int code = ((Integer) responseCode.invoke(response)).intValue();
                    status(connection, code);
                } catch (ReflectiveOperationException | RuntimeException e) {
                    line("instrumentation_error engine=system-http stage=response");
                }
            } else if ("disconnect".equals(kind)) {
                disconnect(connection);
            }
            return result;
        } catch (InvocationTargetException e) {
            if ("disconnect".equals(kind)) disconnect(connection);
            else failure(connection, kind, e.getCause());
            throw e.getCause();
        } catch (IllegalAccessException | IllegalArgumentException e) {
            line("instrumentation_error engine=system-http stage=invoke_" + kind);
            throw e;
        }
    }

    private static synchronized void start(Object connection) {
        if (active.containsKey(connection)) return;
        if (active.size() >= MAX_ACTIVE) {
            line("capacity_exceeded engine=system-http");
            return;
        }
        Record r = new Record(++nextId);
        active.put(connection, r);
        line("engine=system-http id=" + r.id + " event=start");
    }

    private static synchronized void status(Object connection, int code) {
        Record r = active.get(connection);
        if (r == null || r.reported) return;
        r.reported = true;
        line("engine=system-http id=" + r.id + " event=response http=" +
                (code >= 100 && code <= 599 ? code : 0) + " body_bytes_read=unavailable");
    }

    private static synchronized void failure(Object connection, String phase, Throwable error) {
        Record r = active.get(connection);
        if (r == null || r.reported) return;
        r.reported = true;
        String origin = "not_sampled";
        if (originSamples < 8) {
            originSamples++;
            origin = causeOrigin(error);
        }
        String checked = checkedDetails(error);
        line("engine=system-http id=" + r.id + " event=failure phase=" + phase + " error=" +
                MapsNetworkDiagnostics.errorKind(error) + " cause_types=" + causeTypes(error) +
                " os_errors=" + systemErrors(error) + " body_bytes_read=unavailable cause_origin=" + origin + checked);
    }

    private static String checkedDetails(Throwable error) {
        try {
            if (!(error instanceof IOException)) return "";
            Throwable cause = error.getCause();
            if (cause == null || !"checked_other".equals(causeType(cause))) return "";
            if (checkedSamples++ >= MAX_CHECKED_SAMPLES) return " checked_sample=limit_reached";

            Class<?> type = cause.getClass();
            Integer group = checkedGroups.get(type);
            if (group == null && checkedGroups.size() < MAX_CHECKED_GROUPS) {
                group = checkedGroups.size() + 1;
                checkedGroups.put(type, group);
            }
            String name = type.getName();
            String scope = name.startsWith("com.android.okhttp.") ? "system_http"
                    : name.startsWith("com.android.org.conscrypt.") || name.startsWith("org.conscrypt.") ? "system_tls"
                    : name.startsWith("com.google.android.gms.") ? "gms"
                    : name.startsWith("java.") || name.startsWith("javax.") || name.startsWith("android.") ? "platform"
                    : "other";
            boolean providerFrame = false;
            StackTraceElement[] frames = cause.getStackTrace();
            for (int i = 0; i < frames.length && i < 48; i++) {
                String frame = frames[i].getClassName();
                if ("com.google.android.gms.security.ProviderInstaller".equals(frame)
                        || "com.google.android.gms.providerinstaller.ProviderInstallerImpl".equals(frame)
                        || "com.google.android.gms.common.security.ProviderInstallerImpl".equals(frame)) {
                    providerFrame = true;
                    break;
                }
            }
            String provider;
            try {
                provider = Security.getProvider("GmsCore_OpenSSL") == null ? "absent" : "registered";
            } catch (RuntimeException | LinkageError e) {
                provider = "unavailable";
            }
            return " checked_sample=recorded checked_group=" + (group == null ? "overflow" : group)
                    + " checked_scope=" + scope + " provider_frame=" + (providerFrame ? "seen" : "not_seen")
                    + " gms_provider=" + provider;
        } catch (RuntimeException | LinkageError e) {
            return " checked_sample=unavailable";
        }
    }

    private static String causeTypes(Throwable error) {
        StringBuilder types = new StringBuilder();
        int count = 0;
        while (error != null && count++ < 8) {
            if (types.length() != 0) types.append(',');
            types.append(causeType(error));
            error = error.getCause();
        }
        if (error != null) types.append(",truncated");
        return types.length() == 0 ? "none" : types.toString();
    }

    private static String causeType(Throwable error) {
        String name = error.getClass().getName();
        if ("android.system.ErrnoException".equals(name)) return "errno";
        if ("android.system.GaiException".equals(name)) return "gai";
        if (error instanceof CertificateException || error instanceof SSLPeerUnverifiedException) return "certificate";
        if (error instanceof SSLException) return "tls";
        if (error instanceof NoSuchAlgorithmException) return "algorithm";
        if (error instanceof KeyManagementException) return "key_management";
        if (error instanceof KeyStoreException) return "key_store";
        if (error instanceof InvalidKeyException) return "invalid_key";
        if (error instanceof GeneralSecurityException) return "general_security";
        if (error instanceof UnknownHostException) return "dns";
        if (error instanceof SocketTimeoutException) return "timeout";
        if (error instanceof NoRouteToHostException) return "unreachable";
        if (error instanceof ConnectException) return "connect";
        if (error instanceof ProtocolException) return "protocol";
        if (error instanceof EOFException) return "eof";
        if (error instanceof SocketException) return "socket";
        if (error instanceof InterruptedIOException) return "interrupted_io";
        if (error instanceof IOException) return "io";
        if (error instanceof InvocationTargetException) return "reflection_target";
        if (error instanceof IllegalAccessException) return "reflection_access";
        if (error instanceof ClassNotFoundException) return "class_not_found";
        if (error instanceof NoSuchMethodException) return "method_not_found";
        if (error instanceof NoSuchFieldException) return "field_not_found";
        if (error instanceof ReflectiveOperationException) return "reflection_other";
        if (error instanceof ExecutionException) return "execution";
        if (error instanceof InterruptedException) return "interrupted";
        if (error instanceof NullPointerException) return "null_pointer";
        if (error instanceof IllegalStateException) return "illegal_state";
        if (error instanceof IllegalArgumentException) return "illegal_argument";
        if (error instanceof SecurityException) return "security";
        if (error instanceof UnsupportedOperationException) return "unsupported";
        if (error instanceof LinkageError) return "linkage";
        if (error instanceof RuntimeException) return "runtime";
        if (error instanceof Error) return "error";
        if (error.getClass() == Exception.class) return "exception";
        if (error instanceof Exception) return "checked_other";
        return "other";
    }

    private static String causeOrigin(Throwable error) {
        try {
            Throwable last = error;
            for (int i = 0; last != null && i < 7; i++) {
                Throwable next = last.getCause();
                if (next == null || next == last) break;
                last = next;
            }
            if (last == null) return "none";
            StackTraceElement[] frames = last.getStackTrace();
            for (int i = 0; i < frames.length && i < 48; i++) {
                String name = frames[i].getClassName();
                if (name.startsWith("com.android.okhttp.")) return "system_http";
                if (name.startsWith("com.android.org.conscrypt.") || name.startsWith("org.conscrypt.")) return "system_tls";
                if (name.startsWith("javax.net.ssl.")) return "platform_tls";
                if (name.startsWith("libcore.io.") || name.startsWith("android.system.")) return "system_io";
                if (name.startsWith("java.net.") || name.startsWith("sun.nio.ch.")) return "platform_network";
                if (name.startsWith("megler.")) return "megler";
                if (name.startsWith("org.chromium.")) return "chromium";
            }
            return "other";
        } catch (RuntimeException e) {
            return "unavailable";
        }
    }

    private static String systemErrors(Throwable error) {
        StringBuilder codes = new StringBuilder();
        for (int i = 0; error != null && i < 8; i++, error = error.getCause()) {
            String name = error.getClass().getName();
            String field, prefix;
            if ("android.system.ErrnoException".equals(name)) {
                field = "errno";
                prefix = "errno";
            } else if ("android.system.GaiException".equals(name)) {
                field = "error";
                prefix = "gai";
            } else continue;
            if (codes.length() != 0) codes.append(',');
            codes.append(prefix).append('_');
            try {
                int value = error.getClass().getField(field).getInt(error);
                if (value >= -4095 && value <= 4095) codes.append(value);
                else codes.append("out_of_range");
            } catch (ReflectiveOperationException | RuntimeException e) {
                codes.append("unavailable");
            }
        }
        return codes.length() == 0 ? "none" : codes.toString();
    }

    private static synchronized void disconnect(Object connection) {
        Record r = active.remove(connection);
        if (r != null && !r.reported)
            line("engine=system-http id=" + r.id + " event=disconnect");
    }

    private static synchronized void line(String text) {
        if (events++ < MAX_EVENTS) android.util.Log.i("Megler", "maps-net: " + text);
        else if (!overflow) {
            overflow = true;
            android.util.Log.w("Megler", "maps-net: event_limit_reached engine=system-http");
        }
    }
}
