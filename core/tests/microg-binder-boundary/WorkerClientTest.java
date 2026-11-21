import android.os.IBinder;
import android.os.Binder;
import android.os.Parcel;
import android.os.RemoteException;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import megler.BinderBridge;

public class WorkerClientTest {
    private static IBinder bind(IBinder router, int id, String action) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInt(id);
            request.writeString(action);
            request.writeString(null);
            if (!router.transact(1, request, reply, 0)) throw new AssertionError("bind");
            reply.readException();
            if (reply.readString() == null) throw new AssertionError("component");
            IBinder service = reply.readStrongBinder();
            if (service == null) throw new AssertionError("service");
            return service;
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void broker(IBinder service, String packageName, boolean reject) throws Exception {
        CountDownLatch called = new CountDownLatch(1);
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags)
                    throws RemoteException {
                if (code != 1) return false;
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                if (data.readInt() != 0 || data.readStrongBinder() == null) throw new AssertionError("callback");
                response.writeNoException();
                called.countDown();
                return true;
            }
        };
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("com.google.android.gms.common.internal.IGmsServiceBroker");
            request.writeStrongBinder(callback);
            request.writeInt(1);
            request.writeInt(0xffff4f45);
            int sizeAt = request.dataPosition(); request.writeInt(0);
            int start = request.dataPosition();
            request.writeInt(0x40001); request.writeInt(6);
            request.writeInt(0x40002); request.writeInt(39);
            request.writeInt(0x40003); request.writeInt(252432034);
            request.writeInt(0xffff0004);
            int fieldAt = request.dataPosition(); request.writeInt(0);
            int fieldStart = request.dataPosition();
            request.writeString(packageName);
            int end = request.dataPosition();
            request.setDataPosition(fieldAt); request.writeInt(end - fieldStart); request.setDataPosition(end);
            request.setDataPosition(sizeAt); request.writeInt(end - start); request.setDataPosition(end);
            try {
                if (!service.transact(46, request, reply, 0)) throw new AssertionError("broker transact");
            } catch (RemoteException e) {
                if (reject) return;
                throw e;
            }
            if (reject) throw new AssertionError("broker accepted another package");
            reply.readException();
            if (!called.await(10, TimeUnit.SECONDS)) throw new AssertionError("broker callback");
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void advertisingId(IBinder service) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("com.google.android.gms.ads.identifier.internal.IAdvertisingIdService");
            if (!service.transact(1, request, reply, 0)) throw new AssertionError("advertising id");
            reply.readException();
            if (!"00000000-0000-0000-0000-000000000000".equals(reply.readString()))
                throw new AssertionError("unexpected advertising id");
        } finally { request.recycle(); reply.recycle(); }
    }

    public static void main(String[] args) throws Exception {
        FileDescriptor descriptor = new FileDescriptor();
        Field field = FileDescriptor.class.getDeclaredField("fd");
        field.setAccessible(true);
        field.setInt(descriptor, Integer.parseInt(args[0]));
        BinderBridge bridge = new BinderBridge(new FileInputStream(descriptor),
                                                new FileOutputStream(descriptor), null);
        IBinder service = bind(bridge.remoteRoot(), 1, "com.google.android.gms.ads.identifier.service.START");
        advertisingId(service);
        IBinder broker = bind(bridge.remoteRoot(), 2, "com.google.android.gms.common.service.START");
        broker(broker, args[1], false);
        broker(broker, args[2], true);
        System.out.println("BOUND");
        System.out.flush();
        if (System.in.read() == 'c') {
            advertisingId(service);
            broker(broker, args[1], false);
            System.out.println("STILL_BOUND");
            System.out.flush();
            System.in.read();
        }
        System.exit(0);
    }
}
