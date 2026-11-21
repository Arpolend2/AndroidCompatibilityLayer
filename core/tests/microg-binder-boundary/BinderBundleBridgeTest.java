import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import megler.BinderBridge;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

public class BinderBundleBridgeTest {
    public static final class Sample implements Parcelable {
        static int writes;
        final int number;
        final Bundle bundle;
        Sample(int number, Bundle bundle) { this.number = number; this.bundle = bundle; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) {
            writes++;
            dest.writeInt(number);
            dest.writeBundle(bundle);
        }
        public static final Parcelable.Creator<Sample> CREATOR = new Parcelable.Creator<Sample>() {
            @Override public Sample createFromParcel(Parcel in) {
                return new Sample(in.readInt(), in.readBundle());
            }
            @Override public Sample[] newArray(int size) { return new Sample[size]; }
        };
    }

    public static final class Cyclic implements Parcelable {
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) { dest.writeTypedObject(this, flags); }
    }

    public static final class HasFd implements Parcelable {
        @Override public int describeContents() { return Parcelable.CONTENTS_FILE_DESCRIPTOR; }
        @Override public void writeToParcel(Parcel dest, int flags) { throw new AssertionError("FD serialized"); }
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void child() throws Exception {
        Binder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == 4) {
                    Sample[] values = data.createTypedArray(Sample.CREATOR);
                    check(values.length == 3 && values[0].number == 31 && values[1] == null &&
                          values[2].number == 32, "typed array contents");
                    reply.writeTypedArray(values, 0);
                    return true;
                }
                if (code == 3) {
                    Sample sample = data.readTypedObject(Sample.CREATOR);
                    check(sample.number == 31, "parcelable number");
                    IBinder callback = sample.bundle.getBinder("callback");
                    Parcel request = Parcel.obtain(), result = Parcel.obtain();
                    try {
                        request.writeInt(7);
                        check(callback.transact(2, request, result, 0), "parcelable callback");
                        check(result.readInt() == 8, "parcelable callback result");
                    } finally { request.recycle(); result.recycle(); }
                    reply.writeParcelable(sample, 0);
                    return true;
                }
                if (code != 1) return false;
                Bundle bundle = data.readBundle();
                check(bundle != null && "example".equals(bundle.getString("name")), "bundle string");
                check(bundle.getInt("count") == 42 && bundle.getLong("large") == 123456789012L,
                      "bundle numbers");
                check(bundle.getBoolean("enabled"), "bundle boolean");
                check(Arrays.equals(bundle.getByteArray("bytes"), new byte[] {1, 2, 3}), "bundle bytes");
                Bundle nested = bundle.getBundle("nested");
                check(nested != null && "inside".equals(nested.getString("value")), "nested bundle");
                IBinder callback = nested.getBinder("callback");
                Parcel request = Parcel.obtain(), result = Parcel.obtain();
                try {
                    request.writeInt(7);
                    check(callback.transact(2, request, result, 0), "nested callback");
                    check(result.readInt() == 8, "nested callback reply");
                } finally { request.recycle(); result.recycle(); }
                reply.writeBundle(bundle);
                return true;
            }
        };
        BinderBridge bridge = new BinderBridge(System.in, System.out, service);
        bridge.awaitClosed();
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("child")) { child(); return; }
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin"
            + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                            BinderBundleBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        try {
            Binder callback = new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    check(code == 2 && data.readInt() == 7, "callback data");
                    reply.writeInt(8);
                    return true;
                }
            };
            Bundle nested = new Bundle();
            nested.putString("value", "inside");
            nested.putBinder("callback", callback);
            Bundle bundle = new Bundle();
            bundle.putString("name", "example");
            bundle.putInt("count", 42);
            bundle.putLong("large", 123456789012L);
            bundle.putBoolean("enabled", true);
            bundle.putByteArray("bytes", new byte[] {1, 2, 3});
            bundle.putBundle("nested", nested);
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeBundle(bundle);
                check(bridge.remoteRoot().transact(1, data, reply, 0), "bundle transaction");
                Bundle returned = reply.readBundle();
                check(returned != bundle && "example".equals(returned.getString("name")), "bundle copied");
                check(returned.getBundle("nested").getBinder("callback") == callback, "callback identity");
            } finally { data.recycle(); reply.recycle(); }

            Parcel typed = Parcel.obtain(), typedReply = Parcel.obtain();
            try {
                Bundle inside = new Bundle();
                inside.putBinder("callback", callback);
                Sample sample = new Sample(31, inside);
                typed.writeTypedObject(sample, 0);
                check(bridge.remoteRoot().transact(3, typed, typedReply, 0), "parcelable transaction");
                check(Sample.writes == 1, "parcelable serialized once");
                Sample returned = typedReply.readParcelable(Sample.class.getClassLoader());
                check(returned != sample && returned.number == 31, "parcelable copied");
                check(returned.bundle.getBinder("callback") == callback, "parcelable callback identity");
            } finally { typed.recycle(); typedReply.recycle(); }

            Parcel array = Parcel.obtain(), arrayReply = Parcel.obtain();
            try {
                Bundle inside = new Bundle();
                inside.putBinder("callback", callback);
                Sample[] samples = {new Sample(31, inside), null, new Sample(32, inside)};
                array.writeTypedArray(samples, 0);
                check(bridge.remoteRoot().transact(4, array, arrayReply, 0), "typed array transaction");
                Sample[] result = arrayReply.createTypedArray(Sample.CREATOR);
                check(result.length == 3 && result[0].number == 31 && result[1] == null &&
                      result[2].number == 32 && result[0].bundle.getBinder("callback") == callback,
                      "typed array values and nested Binder identity");
            } finally { array.recycle(); arrayReply.recycle(); }

            Parcel arrayFlags = Parcel.obtain();
            try {
                Sample[] values = {new Sample(1, new Bundle())};
                arrayFlags.writeStrongBinder(callback);
                arrayFlags.writeParcelableArray(values, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                arrayFlags.setDataPosition(0);
                check(arrayFlags.readStrongBinder() == callback &&
                      arrayFlags.readParcelableArray(Sample.class.getClassLoader()) == values,
                      "local flagged array identity");
                boolean rejected = false;
                int[] exported = {0};
                try { arrayFlags.meglerMarshallBinders(b -> { exported[0]++; return 1; }); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected && exported[0] == 0, "unsupported array flags rejected before export");
            } finally { arrayFlags.recycle(); }

            Parcel typedArrayFlags = Parcel.obtain();
            try {
                Sample[] values = {new Sample(1, new Bundle())};
                typedArrayFlags.writeTypedArray(values, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                typedArrayFlags.setDataPosition(0);
                check(typedArrayFlags.createTypedArray(Sample.CREATOR) == values,
                      "local flagged typed array identity");
                boolean rejected = false;
                try { typedArrayFlags.meglerMarshallBinders(b -> 1); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected, "unsupported typed array flags rejected");
            } finally { typedArrayFlags.recycle(); }

            Parcel arrayEdges = Parcel.obtain();
            try {
                arrayEdges.writeTypedArray(null, 0);
                arrayEdges.writeTypedArray(new Sample[0], 0);
                arrayEdges.writeParcelableArray(new Parcelable[] {null}, 0);
                arrayEdges.writeValue(new Parcelable[] {new Sample(12, new Bundle())});
                byte[] wire = arrayEdges.meglerMarshallBinders(b -> 1);
                Parcel restored = Parcel.meglerUnmarshallBinders(wire, h -> callback);
                try {
                    check(restored.createTypedArray(Sample.CREATOR) == null, "null array");
                    check(restored.createTypedArray(Sample.CREATOR).length == 0, "empty array");
                    Parcelable[] nulls = restored.readParcelableArray(null);
                    check(nulls.length == 1 && nulls[0] == null, "null-only parcelable array");
                    Parcelable[] values = (Parcelable[]) restored.readValue(null);
                    check(values.length == 1 && ((Sample) values[0]).number == 12,
                          "parcelable array read as value");
                } finally { restored.recycle(); }
            } finally { arrayEdges.recycle(); }

            Parcel oversizedArray = Parcel.obtain();
            try {
                oversizedArray.writeStrongBinder(callback);
                oversizedArray.writeTypedArray(new Sample[4096], 0);
                int[] exported = {0};
                boolean rejected = false;
                try { oversizedArray.meglerMarshallBinders(b -> { exported[0]++; return 1; }); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected && exported[0] == 0, "oversized array rejected before export");
            } finally { oversizedArray.recycle(); }

            Parcel fdArray = Parcel.obtain();
            try {
                fdArray.writeStrongBinder(callback);
                fdArray.writeParcelableArray(new Parcelable[] {new HasFd()}, 0);
                int[] exported = {0};
                boolean rejected = false;
                try { fdArray.meglerMarshallBinders(b -> { exported[0]++; return 1; }); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected && exported[0] == 0, "FD array rejected before export");
            } finally { fdArray.recycle(); }

            Parcel malformedArray = Parcel.obtain();
            try {
                malformedArray.writeStrongBinder(callback);
                malformedArray.writeTypedArray(new Sample[] {null}, 0);
                byte[] wire = malformedArray.meglerMarshallBinders(b -> 1);
                java.nio.ByteBuffer buffer = java.nio.ByteBuffer.wrap(wire);
                int arrayTag = 12 + buffer.getInt(4) + 4 + 1 + 8 + 4;
                check(wire[arrayTag] == 9, "array wire tag");
                byte[] invalidElement = wire.clone();
                invalidElement[arrayTag + 5] = 1;
                byte[] invalidLength = wire.clone();
                java.nio.ByteBuffer.wrap(invalidLength).putInt(arrayTag + 1, 4097);
                for (byte[] bad : new byte[][] {invalidElement, invalidLength}) {
                    int[] imported = {0};
                    boolean rejected = false;
                    try { Parcel.meglerUnmarshallBinders(bad, h -> { imported[0]++; return callback; }); }
                    catch (IllegalArgumentException expected) { rejected = true; }
                    check(rejected && imported[0] == 0, "invalid array rejected before import");
                }
            } finally { malformedArray.recycle(); }

            Parcel parcelable = Parcel.obtain();
            try {
                Bundle inside = new Bundle();
                inside.putBinder("callback", callback);
                parcelable.writeStrongBinder(callback);
                parcelable.writeTypedObject(new Sample(31, inside), 0);
                byte[] wire = parcelable.meglerMarshallBinders(b -> 1);
                byte[] broken = wire.clone();
                int magic = -1;
                for (int i = 4; i < broken.length - 4; i++) {
                    if (broken[i] == 'M' && broken[i + 1] == 'B' &&
                            broken[i + 2] == 'N' && broken[i + 3] == '2') { magic = i; break; }
                }
                check(magic >= 0, "nested parcel header found");
                broken[magic] = 0;
                boolean rejected = false;
                int[] imported = {0};
                try { Parcel.meglerUnmarshallBinders(broken, h -> { imported[0]++; return callback; }); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected && imported[0] == 0, "invalid nested parcel rejected before importing");
                Parcel restored = Parcel.meglerUnmarshallBinders(wire, h -> callback);
                try {
                    restored.readStrongBinder();
                    rejected = false;
                    try { restored.readTypedObject(Bundle.CREATOR); }
                    catch (android.os.BadParcelableException expected) { rejected = true; }
                    check(rejected, "wrong parcelable creator rejected");
                } finally { restored.recycle(); }
            } finally { parcelable.recycle(); }

            Parcel cyclicParcelable = Parcel.obtain();
            try {
                cyclicParcelable.writeTypedObject(new Cyclic(), 0);
                boolean rejected = false;
                try { cyclicParcelable.meglerMarshallBinders(b -> 1); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected, "cyclic parcelable rejected");
            } finally { cyclicParcelable.recycle(); }

            Parcel flagged = Parcel.obtain();
            try {
                Sample sample = new Sample(1, new Bundle());
                flagged.writeStrongBinder(callback);
                flagged.writeTypedObject(sample, Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                flagged.setDataPosition(0);
                check(flagged.readStrongBinder() == callback && flagged.readTypedObject(Sample.CREATOR) == sample,
                      "local flagged parcelable retains identity");
                int[] exported = {0};
                boolean rejected = false;
                try { flagged.meglerMarshallBinders(b -> { exported[0]++; return 1; }); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected && exported[0] == 0, "unsupported flags rejected before export");
            } finally { flagged.recycle(); }

            Parcel invalidText = Parcel.obtain();
            try {
                Bundle b = new Bundle();
                b.putString("bad", "\uD800");
                invalidText.writeStrongBinder(callback);
                invalidText.writeBundle(b);
                int[] exported = {0};
                boolean rejected = false;
                try { invalidText.meglerMarshallBinders(binder -> { exported[0]++; return 1; }); }
                catch (IllegalStateException expected) { rejected = true; }
                check(rejected && exported[0] == 0, "invalid UTF-16 rejected before export");
            } finally { invalidText.recycle(); }

            Parcel unsupported = Parcel.obtain();
            try {
                Bundle b = new Bundle();
                b.putFloat("unsupported", 1.0f);
                unsupported.writeBundle(b);
                boolean rejected = false;
                try { unsupported.meglerMarshallBinders(ignored -> 1); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected, "unsupported bundle value rejected");
            } finally { unsupported.recycle(); }

            Parcel cyclic = Parcel.obtain();
            try {
                Bundle b = new Bundle();
                b.putBundle("self", b);
                cyclic.writeBundle(b);
                boolean rejected = false;
                try { cyclic.meglerMarshallBinders(ignored -> 1); }
                catch (IllegalArgumentException expected) { rejected = true; }
                check(rejected, "cyclic bundle rejected");
            } finally { cyclic.recycle(); }
        } finally {
            bridge.close();
            boolean exited = child.waitFor(5, TimeUnit.SECONDS);
            if (!exited) child.destroyForcibly();
            check(exited && child.exitValue() == 0, "child failed");
        }
        System.out.println("Binder nested Bundle bridge test passed");
    }
}
