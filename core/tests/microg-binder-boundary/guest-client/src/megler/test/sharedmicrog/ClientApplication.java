package megler.test.sharedmicrog;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.util.Log;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ClientApplication extends Application {
    private static final String TAG = "SharedMicrogUidTest";

    @Override public void onCreate() {
        super.onCreate();
        try {
            int uid = Process.myUid();
            if (uid < 11000 || uid > 19999 || Binder.getCallingUid() != uid ||
                    getApplicationInfo().uid != uid ||
                    getPackageManager().getPackageUid(getPackageName(), 0) != uid ||
                    !Arrays.equals(getPackageManager().getPackagesForUid(uid), new String[] { getPackageName() }))
                throw new AssertionError("inconsistent client UID");
            Log.i(TAG, "UID " + getPackageName() + " " + uid);
            Connection advertising = new Connection();
            Connection broker = new Connection();
            if (!bindService(intent("com.google.android.gms.ads.identifier.service.START"), advertising, Context.BIND_AUTO_CREATE) ||
                    !bindService(intent("com.google.android.gms.common.service.START"), broker, Context.BIND_AUTO_CREATE))
                throw new AssertionError("bindService rejected");
            new Thread(() -> runChecks(advertising, broker), "shared-microg-uid-test").start();
        } catch (Throwable failure) {
            Log.e(TAG, "FAIL", failure);
        }
    }

    private Intent intent(String action) {
        return new Intent(action).setPackage("com.google.android.gms");
    }

    private static final class Connection implements ServiceConnection {
        final CountDownLatch ready = new CountDownLatch(1);
        volatile IBinder binder;
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            binder = service;
            ready.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) {}
        IBinder await() throws InterruptedException {
            if (!ready.await(30, TimeUnit.SECONDS) || binder == null) throw new AssertionError("service unavailable");
            return binder;
        }
    }

    private void runChecks(Connection advertising, Connection broker) {
        try {
            advertisingId(advertising.await());
            IBinder service = broker.await();
            broker(service, getPackageName(), false);
            String other = getPackageName().endsWith("1") ? "megler.test.sharedclient2" : "megler.test.sharedclient1";
            broker(service, other, true);
            Log.i(TAG, "PASS " + getPackageName() + " " + Process.myUid());
        } catch (Throwable failure) {
            Log.e(TAG, "FAIL", failure);
        }
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

    private static void broker(IBinder service, String packageName, boolean reject) throws Exception {
        CountDownLatch called = new CountDownLatch(1);
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags)
                    throws RemoteException {
                if (code != 1) return false;
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                if (data.readInt() != 0 || data.readStrongBinder() == null) throw new AssertionError("broker callback");
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
                if (!service.transact(46, request, reply, 0)) throw new AssertionError("broker transaction");
                reply.readException();
            } catch (RemoteException | SecurityException e) {
                if (reject) return;
                throw e;
            }
            if (reject) throw new AssertionError("broker accepted another package");
            if (!called.await(10, TimeUnit.SECONDS)) throw new AssertionError("broker callback missing");
        } finally { request.recycle(); reply.recycle(); }
    }
}
