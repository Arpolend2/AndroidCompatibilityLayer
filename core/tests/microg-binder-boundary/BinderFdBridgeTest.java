import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import megler.BinderBridge;

public class BinderFdBridgeTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 1, "expected client or server");
        FileDescriptor descriptor = new FileDescriptor();
        Field field = FileDescriptor.class.getDeclaredField("fd");
        field.setAccessible(true);
        field.setInt(descriptor, 3);
        check(descriptor.valid(), "inherited stream descriptor");
        FileInputStream input = new FileInputStream(descriptor);
        FileOutputStream output = new FileOutputStream(descriptor);
        if ("server".equals(args[0]) || "server-drop".equals(args[0])) {
            final boolean drop = "server-drop".equals(args[0]);
            Binder service = new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                        throws RemoteException {
                    if (drop && code == 3) System.exit(0);
                    if (code == 6) { System.exit(0); return true; }
                    if (code == 4) {
                        check(flags == IBinder.FLAG_ONEWAY && reply == null, "one-way server dispatch");
                        IBinder callback = data.readStrongBinder();
                        Parcel request = Parcel.obtain();
                        try {
                            request.writeInt(19);
                            check(callback.transact(5, request, null, IBinder.FLAG_ONEWAY), "one-way callback accepted");
                        } finally { request.recycle(); }
                        return true;
                    }
                    if (code != 1) return false;
                    check(data.readInt() == 42, "request value");
                    IBinder callback = data.readStrongBinder();
                    Parcel request = Parcel.obtain(), result = Parcel.obtain();
                    try {
                        request.writeInt(7);
                        check(callback.transact(2, request, result, 0), "callback transaction");
                        check(result.readInt() == 8, "callback result");
                    } finally { request.recycle(); result.recycle(); }
                    reply.writeStrongBinder(callback);
                    reply.writeInt(43);
                    return true;
                }
            };
            BinderBridge bridge = new BinderBridge(input, output, service);
            bridge.awaitClosed();
        } else {
            final boolean drop = "client-drop".equals(args[0]);
            check(drop || "client".equals(args[0]), "invalid mode");
            CountDownLatch oneWayCallback = new CountDownLatch(1);
            Binder callback = new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    if (code == 5) {
                        check(flags == IBinder.FLAG_ONEWAY && reply == null && data.readInt() == 19,
                              "one-way callback");
                        oneWayCallback.countDown();
                        return true;
                    }
                    check(code == 2 && data.readInt() == 7, "callback request");
                    reply.writeInt(8);
                    return true;
                }
            };
            BinderBridge bridge = new BinderBridge(input, output, null);
            try {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    if (drop) {
                        boolean failed = false;
                        try { bridge.remoteRoot().transact(3, data, reply, 0); }
                        catch (RemoteException expected) { failed = true; }
                        check(failed, "peer exit fails the pending call");
                    } else {
                        data.writeInt(42);
                        data.writeStrongBinder(callback);
                        check(bridge.remoteRoot().transact(1, data, reply, 0), "remote transaction");
                        check(reply.readStrongBinder() == callback && reply.readInt() == 43,
                              "reply and callback identity");
                        Parcel oneWay = Parcel.obtain();
                        try {
                            oneWay.writeStrongBinder(callback);
                            check(bridge.remoteRoot().transact(4, oneWay, null, IBinder.FLAG_ONEWAY),
                                  "one-way request accepted");
                        } finally { oneWay.recycle(); }
                        check(oneWayCallback.await(5, TimeUnit.SECONDS), "one-way callback delivered");
                    }
                } finally { data.recycle(); reply.recycle(); }
            } finally { bridge.close(); }
            System.out.println(drop ? "Binder D-Bus peer disconnect passed" : "Binder D-Bus descriptor bridge passed");
        }
    }
}
