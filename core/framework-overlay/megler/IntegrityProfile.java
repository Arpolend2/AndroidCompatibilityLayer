// An opt-in profile for a hosted DroidGuard, following PlayIntegrityFork's three default modes.
// GMS runs inside the app here: Build and native properties belong to that whole process.
package megler;

import android.os.Build;
import android.os.SystemProperties;
import android.util.Log;
import java.io.*;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.*;
import org.json.JSONObject;

public final class IntegrityProfile {
    private static volatile boolean blockCertificateChain;
    private static final String[][] BUILD_PROPERTIES = {
        { "FINGERPRINT", "ro.build.fingerprint" }, { "ID", "ro.build.id" },
        { "DISPLAY", "ro.build.display.id" }, { "TYPE", "ro.build.type" },
        { "TAGS", "ro.build.tags" }, { "MODEL", "ro.product.model" },
        { "MANUFACTURER", "ro.product.manufacturer" }, { "BRAND", "ro.product.brand" },
        { "PRODUCT", "ro.product.name" }, { "DEVICE", "ro.product.device" },
        { "HARDWARE", "ro.hardware" }, { "RELEASE", "ro.build.version.release" },
        { "INCREMENTAL", "ro.build.version.incremental" },
        { "SECURITY_PATCH", "ro.build.version.security_patch" },
        { "SDK_INT", "ro.build.version.sdk" }
    };

    private IntegrityProfile() {}

    /** Called before Application.onCreate. The profile is private to this Android app's data directory. */
    public static synchronized void install() {
        File file = profileFile();
        if (file == null) {
            blockCertificateChain = false;
            return;
        }
        ArrayList<FieldChange> changes = new ArrayList<>();
        LinkedHashMap<String, String> properties = new LinkedHashMap<>();
        try {
            Map<String, String> profile = read(file);
            // These optional upstream modes need other package boundaries than the three default modes.
            for (String option : new String[] { "spoofSignature", "spoofVendingSdk", "spoofVendingFinger" }) {
                if (!profile.getOrDefault(option, "0").equals("0"))
                    throw new IllegalArgumentException("Unsupported option " + option);
            }
            boolean build = enabled(profile, "spoofBuild");
            boolean props = enabled(profile, "spoofProps");
            boolean provider = enabled(profile, "spoofProvider");
            for (Map.Entry<String, String> entry : profile.entrySet()) {
                String name = entry.getKey(), value = entry.getValue();
                if (value.isEmpty() || name.startsWith("spoof") || name.equals("verboseLogs")) continue;
                if (name.startsWith("*"))
                    throw new IllegalArgumentException("Wildcard properties require explicit names");
                if (name.indexOf('.') >= 0) {
                    if (props) properties.put(name, value);
                } else if (build) {
                    Field field = buildField(name);
                    if (field == null) throw new IllegalArgumentException("Unknown Build field " + name);
                    changes.add(new FieldChange(field, value));
                    if (props)
                        for (String[] pair : BUILD_PROPERTIES)
                            if (pair[0].equals(name) && profile.getOrDefault(pair[1], "").isEmpty()) properties.put(pair[1], value);
                }
            }
            for (String value : properties.values())
                if (value.getBytes("UTF-8").length > 91) throw new IllegalArgumentException("Property value exceeds 91 bytes");
            for (FieldChange change : changes) change.field.set(null, change.value);
            for (Map.Entry<String, String> entry : properties.entrySet()) {
                SystemProperties.set(entry.getKey(), entry.getValue());
                Native.setSystemProperty(entry.getKey(), entry.getValue());
            }
            blockCertificateChain = provider;
            Log.i("IntegrityProfile", "Applied " + changes.size() + " Build fields and " + properties.size()
                    + " process properties; certificate-chain block " + provider);
        } catch (Exception error) {
            for (FieldChange change : changes) {
                try { change.field.set(null, change.previous); }
                catch (Exception restore) { Log.w("IntegrityProfile", "Could not restore " + change.field.getName(), restore); }
            }
            Log.w("IntegrityProfile", "Profile could not be applied", error);
        }
    }

    private static File profileFile() {
        for (String name : new String[] { "custom.pif.prop", "custom.pif.json", "pif.prop", "pif.json" }) {
            File file = new File(Native.dataDir(), name);
            if (file.isFile()) return file;
        }
        return null;
    }

    private static boolean enabled(Map<String, String> profile, String name) {
        String value = profile.getOrDefault(name, "1");
        if (!value.equals("0") && !value.equals("1")) throw new IllegalArgumentException("Expected 0 or 1 for " + name);
        return value.equals("1");
    }

    private static Map<String, String> read(File file) throws Exception {
        if (file.length() > 65536) throw new IOException("Profile exceeds 64 KiB");
        StringBuilder text = new StringBuilder();
        LinkedHashMap<String, String> result = new LinkedHashMap<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(file), "UTF-8"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (text.length() + line.length() > 65536) throw new IOException("Profile exceeds 64 KiB");
                text.append(line).append('\n');
            }
        }
        if (text.length() > 0 && text.charAt(0) == '\ufeff') text.deleteCharAt(0);
        if (file.getName().endsWith(".json")) {
            JSONObject json = new JSONObject(text.toString());
            Iterator<String> keys = json.keys();
            while (keys.hasNext()) {
                String name = keys.next();
                if (!json.isNull(name)) result.put(name, String.valueOf(json.get(name)).trim());
            }
        } else {
            for (String line : text.toString().split("\n")) {
                int comment = line.indexOf('#');
                if (comment >= 0) line = line.substring(0, comment);
                line = line.trim();
                if (line.isEmpty()) continue;
                int equal = line.indexOf('=');
                if (equal <= 0) throw new IllegalArgumentException("Expected name=value");
                result.put(line.substring(0, equal).trim(), line.substring(equal + 1).trim());
            }
        }
        return result;
    }

    private static Field buildField(String name) throws Exception {
        try { return Build.class.getField(name); }
        catch (NoSuchFieldException absent) {
            try { return Build.VERSION.class.getField(name); }
            catch (NoSuchFieldException missing) { return null; }
        }
    }

    private static final class FieldChange {
        final Field field;
        final Object value, previous;
        FieldChange(Field field, String text) throws Exception {
            if (!Modifier.isStatic(field.getModifiers())) throw new IllegalArgumentException("Expected a static Build field");
            this.field = field;
            field.setAccessible(true);
            previous = field.get(null);
            Class<?> type = field.getType();
            if (type == String.class) value = text;
            else if (type == int.class) value = Integer.valueOf(text);
            else if (type == long.class) value = Long.valueOf(text);
            else if (type == boolean.class && (text.equals("true") || text.equals("false"))) value = Boolean.valueOf(text);
            else throw new IllegalArgumentException("Unsupported Build field type for " + field.getName());
        }
    }

    static boolean blocksCertificateChain() {
        if (!blockCertificateChain) return false;
        for (StackTraceElement frame : Thread.currentThread().getStackTrace())
            if (frame.getClassName().toLowerCase(Locale.ROOT).contains("droidguard")) return true;
        return false;
    }
}
