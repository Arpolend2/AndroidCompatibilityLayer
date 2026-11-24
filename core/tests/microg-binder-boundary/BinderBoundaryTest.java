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

    private static void wireChild() {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(42);
            p.writeStrongBinder(new Binder());
            p.writeStrongBinder(null);
            System.out.println(Base64.getEncoder().encodeToString(p.meglerMarshallBinders(b -> 7)));
        } finally { p.recycle(); }
    }

    private static void binderWire() throws Exception {
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin"
            + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                           BinderBoundaryTest.class.getName(), "wire-child")
            .redirectErrorStream(true).start();
        String line, extra;
        try (BufferedReader out = new BufferedReader(new InputStreamReader(child.getInputStream(), "UTF-8"))) {
            line = out.readLine();
            extra = out.readLine();
        }
        check(child.waitFor() == 0 && line != null && extra == null, "wire child failed");
        byte[] wire = Base64.getDecoder().decode(line);
        Binder resolved = new Binder();
        Parcel received = Parcel.meglerUnmarshallBinders(wire, h -> h == 7 ? resolved : null);
        try {
            check(received.readInt() == 42, "wire primitive");
            check(received.readStrongBinder() == resolved, "wire binder handle");
            check(received.readStrongBinder() == null, "wire null binder");
        } finally { received.recycle(); }

        boolean rejected = false;
        try { Parcel.meglerUnmarshallBinders(wire, h -> null); }
        catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected, "unknown handle rejected");
        rejected = false;
        try { Parcel.meglerUnmarshallBinders(Arrays.copyOf(wire, wire.length - 1), h -> resolved); }
        catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected, "truncated wire rejected");
        byte[] corrupt = wire.clone();
        corrupt[12 + 4] ^= 1;
        rejected = false;
        final int[] imported = {0};
        try { Parcel.meglerUnmarshallBinders(corrupt, h -> { imported[0]++; return resolved; }); }
        catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected && imported[0] == 0, "invalid reference rejected before resolving handles");
        Parcel unsupported = Parcel.obtain();
        try {
            unsupported.writeStrongBinder(resolved);
            unsupported.writeList(new ArrayList<Object>(Arrays.asList("item", 1)));
            rejected = false;
            final int[] exported = {0};
            try { unsupported.meglerMarshallBinders(b -> { exported[0]++; return 1; }); }
            catch (IllegalArgumentException expected) { rejected = true; }
            check(rejected && exported[0] == 0, "unsupported object rejected before exporting handles");
        } finally { unsupported.recycle(); }
    }

    private static void serviceCall() throws Exception {
        final String descriptor = "com.google.android.gms.common.internal.IGmsServiceBroker";
        final String callbackDescriptor = "com.google.android.gms.common.internal.IGmsCallbacks";
        final int[] delivered = {0};
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != 1) return false;
                data.enforceInterface(callbackDescriptor);
                check(data.readInt() == 0 && "synthetic-result".equals(data.readString()), "callback result");
                delivered[0]++;
                if (reply != null) reply.writeNoException();
                return true;
            }
        };
        Binder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws android.os.RemoteException {
                if (code != 46) return false;
                data.enforceInterface(descriptor);
                check(data.readInt() == 39, "service id");
                check(data.readStrongBinder() != null, "existing binder");
                IBinder target = data.readStrongBinder();
                Parcel result = Parcel.obtain();
                Parcel response = Parcel.obtain();
                try {
                    result.writeInterfaceToken(callbackDescriptor);
                    result.writeInt(0);
                    result.writeString("synthetic-result");
                    check(target.transact(1, result, response, 0), "callback transaction");
                    response.readException();
                } finally {
                    result.recycle();
                    response.recycle();
                }
                reply.writeNoException();
                return true;
            }
        };
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken(descriptor);
            request.writeInt(39);
            request.writeStrongBinder(new Binder());
            Parcel inner = Parcel.obtain();
            try {
                inner.writeStrongBinder(callback);
                request.appendFrom(inner, 0, inner.dataSize());
            } finally { inner.recycle(); }
            check(service.transact(46, request, reply, 0), "service transaction");
            reply.readException();
            check(delivered[0] == 1, "callback delivered");
        } finally {
            request.recycle();
            reply.recycle();
        }
        request = Parcel.obtain();
        reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("wrong.interface");
            request.writeInt(39);
            request.writeStrongBinder(new Binder());
            request.writeStrongBinder(callback);
            boolean rejected = false;
            try { service.transact(46, request, reply, 0); }
            catch (SecurityException expected) { rejected = true; }
            check(rejected && delivered[0] == 1, "wrong interface must be rejected before callback");
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    private static void appendCases(byte[] foreignBytes) {
        Binder first = new Binder(), second = new Binder();
        Parcel source = Parcel.obtain(), destination = Parcel.obtain();
        try {
            source.writeInt(123);
            source.writeStrongBinder(second);
            source.writeStrongBinder(null);
            destination.writeStrongBinder(first);
            destination.appendFrom(source, 4, source.dataSize() - 4);
            destination.setDataPosition(0);
            check(destination.readStrongBinder() == first, "original object");
            check(destination.readStrongBinder() == second, "appended object");
            check(destination.readStrongBinder() == null, "appended null");
            byte[] bytes = destination.marshall();
            Parcel restored = Parcel.obtain(), copy = Parcel.obtain();
            try {
                restored.unmarshall(bytes, 0, bytes.length);
                copy.writeStrongBinder(first);
                copy.appendFrom(restored, 8, restored.dataSize() - 8);
                copy.setDataPosition(0);
                check(copy.readStrongBinder() == first, "restored prefix");
                check(copy.readStrongBinder() == second, "restored appended object");
                check(copy.readStrongBinder() == null, "restored appended null");
            } finally { restored.recycle(); copy.recycle(); }
                int before = destination.dataPosition();
                destination.appendFrom(source, 8, 0);
                check(destination.dataPosition() == before, "empty range");
            boolean rejected = false;
            try { destination.appendFrom(source, 8, 4); }
            catch (IllegalArgumentException expected) { rejected = true; }
            check(rejected, "partial object must be rejected");
        } finally { source.recycle(); destination.recycle(); }

        Parcel self = Parcel.obtain();
        try {
            self.writeStrongBinder(first);
            self.appendFrom(self, 0, self.dataSize());
            self.setDataPosition(0);
            check(self.readStrongBinder() == first && self.readStrongBinder() == first, "self append");
        } finally { self.recycle(); }

        Parcel overwritten = Parcel.obtain(), copied = Parcel.obtain();
        try {
            overwritten.writeStrongBinder(second);
            overwritten.setDataPosition(0);
            overwritten.writeInt(0x8c0ffee1);
            overwritten.writeInt(0);
            copied.writeStrongBinder(first);
            copied.appendFrom(overwritten, 0, overwritten.dataSize());
            copied.setDataPosition(0);
            check(copied.readStrongBinder() == first, "original object after overwrite");
            check(copied.readInt() == 0x8c0ffee1 && copied.readInt() == 0, "plain words must not be rewritten");
            copied.setDataPosition(8);
            check(copied.readStrongBinder() == null, "overwritten reference cannot resolve");
        } finally { overwritten.recycle(); copied.recycle(); }

        Parcel foreign = Parcel.obtain(), local = Parcel.obtain();
        try {
            foreign.unmarshall(foreignBytes, 0, foreignBytes.length);
            local.writeStrongBinder(first);
            local.appendFrom(foreign, 0, foreign.dataSize());
            local.setDataPosition(0);
            check(local.readStrongBinder() == first, "local binder before foreign data");
            check(local.readInt() == 42 && "non-secret".equals(local.readString()), "foreign primitive data");
            check(local.readStrongBinder() == null, "foreign index must not resolve to local binder");
        } finally { foreign.recycle(); local.recycle(); }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("child")) {
            child();
            return;
        }
        if (args.length == 1 && args[0].equals("wire-child")) {
            wireChild();
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
        appendCases(bytes);

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
        serviceCall();
        binderWire();
        System.out.println("Binder boundary tests passed; ordinary Parcel marshalling does not transport Binder objects");
    }
}
