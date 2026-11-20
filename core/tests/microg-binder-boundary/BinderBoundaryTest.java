import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;

public class BinderBoundaryTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static byte[] payload(IBinder callback) {
        Parcel p = Parcel.obtain();
        p.writeInt(42);
        p.writeString("non-secret");
        p.writeStrongBinder(callback);
        p.writeList(new ArrayList<String>(Arrays.asList("item")));
        byte[] bytes = p.marshall();
        p.recycle();
        return bytes;
    }

    private static void child() {
        System.out.println(Base64.getEncoder().encodeToString(payload(new Binder())));
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("child")) {
            child();
            return;
        }
        check(args.length == 0, "unexpected arguments");
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin"
            + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                           BinderBoundaryTest.class.getName(), "child")
            .redirectErrorStream(true).start();
        String line;
        String extra;
        try (BufferedReader out = new BufferedReader(new InputStreamReader(child.getInputStream(), "UTF-8"))) {
            line = out.readLine();
            extra = out.readLine();
        }
        check(child.waitFor() == 0 && line != null && extra == null, "child process failed");
        byte[] bytes = Base64.getDecoder().decode(line);
        Parcel received = Parcel.obtain();
        received.unmarshall(bytes, 0, bytes.length);
        check(received.readInt() == 42 && "non-secret".equals(received.readString()),
              "plain data should survive between processes");
        check(received.readStrongBinder() == null, "Binder must not be mistaken for a transported reference");
        check(received.readArrayList(null) == null, "the sender's object table is not transported");
        received.recycle();

        Binder callback = new Binder();
        bytes = payload(callback);
        Parcel local = Parcel.obtain();
        local.unmarshall(bytes, 0, bytes.length);
        check(local.readInt() == 42 && "non-secret".equals(local.readString()), "local plain data");
        check(local.readStrongBinder() == callback, "local marshalling retains Binder identity");
        check(Arrays.asList("item").equals(local.readArrayList(null)), "local marshalling retains objects");
        local.recycle();

        Binder endpoint = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != 1) return false;
                reply.writeStrongBinder(data.readStrongBinder());
                return true;
            }
        };
        Parcel request = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        request.writeStrongBinder(callback);
        check(endpoint.transact(1, request, reply, 0), "local transaction failed");
        check(reply.readStrongBinder() == callback, "local transaction preserves callback identity");
        request.recycle();
        reply.recycle();
        System.out.println("Binder boundary tests passed; no cross-process Binder transport exists");
    }
}
