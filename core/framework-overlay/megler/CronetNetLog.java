package megler;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/*
 * Megler: the flag cronet-netlog. Cronet's own network log (its NetLog, what chrome://net-export writes) of an app that
 * carries Cronet in its own code, as YouTube does (libcronet.so and org.chromium.net.impl): what goes out and comes back
 * is otherwise sealed inside the library's own TLS, and a server's refusal ("Request contains an invalid argument", a
 * 400 on YouTube's channel pages) says nothing of the request it refused. Each CronetUrlRequestContext the app makes
 * starts logging as it is made, with everything (logAll: the bytes of every request and response, cookies and
 * credentials included -- the log stays in the app's data, for the diagnosis alone), into files/netlog-<n>/ of the app's
 * data, at most 64 MB for each (Cronet keeps the newest). Cronet writes it as it goes, in netlog.json.inprogress/; the
 * app ending without stopNetLog leaves it there unstitched. Nothing of the kind without the flag.
 */
final class CronetNetLog {
    private static final String CONTEXT = "org.chromium.net.impl.CronetUrlRequestContext";
    private static final int MOST = 64 << 20;
    private static int sMade;

    private Method mOriginal;

    private CronetNetLog() {}

    /** With the flag: the app's CronetUrlRequestContext constructors hooked, through `loader` (the app's), before its code runs. */
    static void install(ClassLoader loader) {
        if (!Native.flag("cronet-netlog")) return;
        Class<?> c;
        try {
            c = Class.forName(CONTEXT, false, loader);
        } catch (ClassNotFoundException e) {
            android.util.Log.i("Megler", "cronet-netlog: the app has no Cronet of its own (" + CONTEXT + ")");
            return;
        }
        try {
            Method callback = CronetNetLog.class.getDeclaredMethod("callback", Object[].class);
            for (Constructor<?> k : c.getDeclaredConstructors()) {
                CronetNetLog h = new CronetNetLog();
                h.mOriginal = Hooks.hook0(h, k, callback);
                if (h.mOriginal != null) h.mOriginal.setAccessible(true);
                android.util.Log.i("Megler", "cronet-netlog: " + k + (h.mOriginal != null ? " hooked" : " not hooked"));
            }
        } catch (Throwable t) {
            android.util.Log.w("Megler", "cronet-netlog: " + CONTEXT + " not hooked", t);
        }
    }

    /** A constructor's call: the original on the object, then the object's log started. */
    Object callback(Object[] args) {
        Object self = args[0];
        Object[] rest = new Object[args.length - 1];
        System.arraycopy(args, 1, rest, 0, rest.length);
        try {
            mOriginal.invoke(self, rest);
        } catch (InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof RuntimeException) throw (RuntimeException) t;
            if (t instanceof Error) throw (Error) t;
            throw new RuntimeException(t);
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
        start(self);
        return null;
    }

    private static void start(Object context) {
        int n;
        synchronized (CronetNetLog.class) { n = sMade++; }
        File dir = new File(ContextImpl.app().getFilesDir(), "netlog-" + n);
        try {
            dir.mkdirs();
            context.getClass().getMethod("startNetLogToDisk", String.class, boolean.class, int.class).invoke(context, dir.getPath(), true, MOST);
            android.util.Log.i("Megler", "cronet-netlog: Cronet's network log of " + context.getClass().getName() + "@"
                + Integer.toHexString(System.identityHashCode(context)) + " goes to " + dir + " (everything, at most " + (MOST >> 20) + " MB)");
        } catch (Throwable t) {
            android.util.Log.w("Megler", "cronet-netlog: the log of " + context + " did not start", t);
        }
    }
}
