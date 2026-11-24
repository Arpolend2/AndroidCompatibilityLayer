package megler.test.binderfd;

import android.app.Activity;
import android.content.Context;
import android.database.sqlite.SQLiteDatabase;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.RemoteCallbackList;
import android.util.Log;
import android.widget.TextView;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import megler.BinderBridge;
import megler.SharedMicrogServiceRouter;

public class Main extends Activity {
    private static native int takeTestStream();
    private static native boolean isTestServer();
    private static native boolean isTestBroker();
    private static native boolean isTestReal();
    private static native boolean isTestSelfHosted();
    private static native int testCallerUid();
    private static native int testCallerPid();

    private static BinderBridge bridgeFor(ParcelFileDescriptor pfd, IBinder root, int uid, int pid) throws IOException {
        return new BinderBridge(new FileInputStream(pfd.getFileDescriptor()),
            new FileOutputStream(pfd.getFileDescriptor()), root, () -> {
                try { Os.shutdown(pfd.getFileDescriptor(), OsConstants.SHUT_RDWR); }
                catch (ErrnoException e) { throw new IOException("socket shutdown failed", e); }
            }, uid, pid);
    }

    private static void runServer(Context context, ParcelFileDescriptor pfd) throws Exception {
        BinderBridge[] bridge = new BinderBridge[1];
        CountDownLatch ready = new CountDownLatch(1);
        boolean broker = isTestBroker();
        if (broker && isTestReal()) checkGuestSqlitePaths();
        SharedMicrogServiceRouter router = broker ? new SharedMicrogServiceRouter(context) : null;
        Binder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == 6) {
                    if (flags != IBinder.FLAG_ONEWAY) throw new IllegalStateException("stop flags");
                    try { ready.await(); }
                    catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new IllegalStateException("server initialization interrupted", e);
                    }
                    bridge[0].close();
                    return true;
                }
                if (code == 4) {
                    if (flags != IBinder.FLAG_ONEWAY || reply != null)
                        throw new IllegalStateException("one-way shape");
                    IBinder callback = data.readStrongBinder();
                    Parcel oneWay = Parcel.obtain();
                    try {
                        oneWay.writeInt(19);
                        if (!callback.transact(5, oneWay, null, IBinder.FLAG_ONEWAY))
                            throw new IllegalStateException("one-way callback");
                    } finally { oneWay.recycle(); }
                    return true;
                }
                if (code == 7) {
                    if (router == null) return false;
                    if (testCallerUid() >= 0) {
                        if (Binder.getCallingUid() != testCallerUid() || Binder.getCallingPid() != testCallerPid())
                            throw new IllegalStateException("manager caller identity did not reach Binder");
                        Log.i("BinderGuestTest", "PASS manager-bound Binder UID and PID");
                    }
                    reply.writeStrongBinder(router.root());
                    return true;
                }
                if (code != 1 || data.readInt() != 42) return false;
                IBinder callback = data.readStrongBinder();
                Parcel request = Parcel.obtain(), result = Parcel.obtain();
                try {
                    request.writeInt(7);
                    if (!callback.transact(2, request, result, 0) || result.readInt() != 8)
                        throw new IllegalStateException("callback result");
                } finally { request.recycle(); result.recycle(); }
                reply.writeStrongBinder(callback);
                reply.writeInt(43);
                return true;
            }
        };
        int uid = testCallerUid();
        int pid = testCallerPid();
        if (uid < 0 && isTestSelfHosted()) {
            uid = context.getPackageManager().getPackageUid("megler.test.binderfd", 0);
            pid = 7001;
        }
        bridge[0] = bridgeFor(pfd, service, uid, pid);
        ready.countDown();
        bridge[0].awaitClosed();
        if (router != null) router.close();
        Log.i("BinderGuestTest", "PASS guest server closed");
    }

    public static void startHeadless(Context context) {
        new Thread(() -> {
            try {
                if (!isTestServer()) throw new AssertionError("headless server role required");
                if (context.getPackageManager().getPackagesForUid(-1) != null ||
                    context.getPackageManager().getNameForUid(-1) != null)
                    throw new AssertionError("unknown UID mapped to a package");
                int fd = takeTestStream();
                if (fd < 0 || takeTestStream() != -1) throw new AssertionError("test stream ownership");
                Log.i("BinderGuestTest", "PASS headless application started");
                runServer(context, ParcelFileDescriptor.adoptFd(fd));
            } catch (Throwable error) {
                Log.e("BinderGuestTest", "FAIL", error);
            }
        }, "binder-headless-test").start();
    }

    private static void checkGuestSqlitePaths() {
        SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase("/tmp/binder-main.db", null);
        try {
            db.execSQL("CREATE TABLE probe (value INTEGER)");
            db.execSQL("INSERT INTO probe VALUES (7)");
            db.execSQL("ATTACH DATABASE '/tmp/binder-attached.db' AS attached");
            db.execSQL("CREATE TABLE attached.probe (value INTEGER)");
            db.execSQL("INSERT INTO attached.probe VALUES (9)");
            android.database.Cursor attached = db.rawQuery("SELECT value FROM attached.probe", null);
            try {
                if (!attached.moveToFirst() || attached.getInt(0) != 9)
                    throw new AssertionError("attached database contents");
            } finally { attached.close(); }
            db.execSQL("DETACH DATABASE attached");
        } finally { db.close(); }
        db = SQLiteDatabase.openDatabase("/tmp/binder-main.db", null, SQLiteDatabase.OPEN_READONLY);
        try {
            android.database.Cursor cursor = db.rawQuery("SELECT value FROM probe", null);
            try {
                if (!cursor.moveToFirst() || cursor.getInt(0) != 7)
                    throw new AssertionError("guest database contents");
            } finally { cursor.close(); }
        } finally { db.close(); }
        db = SQLiteDatabase.create(null);
        try {
            db.execSQL("CREATE TABLE probe (value INTEGER)");
            db.execSQL("INSERT INTO probe VALUES (11)");
            android.database.Cursor cursor = db.rawQuery("SELECT value FROM probe", null);
            try {
                if (!cursor.moveToFirst() || cursor.getInt(0) != 11)
                    throw new AssertionError("in-memory database contents");
            } finally { cursor.close(); }
        } finally { db.close(); }
        Log.i("BinderGuestTest", "PASS guest SQLite paths and attach");
        db = SQLiteDatabase.openDatabase("/tmp/binder-wal.db", null, 0);
        try {
            android.database.Cursor cursor = db.rawQuery("SELECT value FROM probe", null);
            try {
                if (!cursor.moveToFirst() || cursor.getInt(0) != 13)
                    throw new AssertionError("WAL recovery contents");
            } finally { cursor.close(); }
        } finally { db.close(); }
        Log.i("BinderGuestTest", "PASS guest SQLite WAL recovery");
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        final TextView result = new TextView(this);
        result.setText("Checking Binder stream");
        setContentView(result);
        new Thread(() -> {
            try {
                int fd = takeTestStream();
                if (fd < 0 || takeTestStream() != -1) throw new AssertionError("test stream ownership");
                ParcelFileDescriptor pfd = ParcelFileDescriptor.adoptFd(fd);
                if (isTestServer()) {
                    runServer(this, pfd);
                    runOnUiThread(() -> result.setText("PASS"));
                    return;
                }
                BinderBridge bridge = bridgeFor(pfd, null, -1, 0);
                try {
                    CountDownLatch oneWayCallback = new CountDownLatch(1);
                    Binder callback = new Binder() {
                        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                            if (code == 5) {
                                if (flags != IBinder.FLAG_ONEWAY || reply != null || data.readInt() != 19)
                                    throw new AssertionError("one-way callback");
                                oneWayCallback.countDown();
                                return true;
                            }
                            if (code != 2 || data.readInt() != 7) throw new AssertionError("callback");
                            reply.writeInt(8);
                            return true;
                        }
                    };
                    Parcel request = Parcel.obtain(), reply = Parcel.obtain();
                    try {
                        request.writeInt(42);
                        request.writeStrongBinder(callback);
                        if (!bridge.remoteRoot().transact(1, request, reply, 0) ||
                            reply.readStrongBinder() != callback || reply.readInt() != 43)
                            throw new AssertionError("transaction");
                        Parcel oneWay = Parcel.obtain();
                        try {
                            oneWay.writeStrongBinder(callback);
                            if (!bridge.remoteRoot().transact(4, oneWay, null, IBinder.FLAG_ONEWAY))
                                throw new AssertionError("one-way request");
                        } finally { oneWay.recycle(); }
                        if (!oneWayCallback.await(5, TimeUnit.SECONDS))
                            throw new AssertionError("one-way callback timeout");
                        IBinder root = bridge.remoteRoot();
                        if (isTestBroker()) {
                            if (isTestReal()) {
                                checkRealService(root);
                                checkRealBroker(root);
                            }
                            else checkBroker(root);
                            checkRouterLifecycle(root);
                        }
                        CountDownLatch died = new CountDownLatch(1);
                        CountDownLatch listDied = new CountDownLatch(1);
                        IInterface listener = () -> root;
                        RemoteCallbackList<IInterface> list = new RemoteCallbackList<IInterface>() {
                            @Override public void onCallbackDied(IInterface object, Object cookie) {
                                if (object == listener && "synthetic".equals(cookie)) listDied.countDown();
                            }
                        };
                        if (!list.register(listener, "synthetic"))
                            throw new AssertionError("register remote callback");
                        IBinder.DeathRecipient recipient = died::countDown;
                        root.linkToDeath(recipient, 0);
                        Parcel stop = Parcel.obtain();
                        try {
                            if (!root.transact(6, stop, null, IBinder.FLAG_ONEWAY))
                                throw new AssertionError("stop request");
                        } finally { stop.recycle(); }
                        if (!died.await(5, TimeUnit.SECONDS)) throw new AssertionError("death recipient timeout");
                        if (!listDied.await(5, TimeUnit.SECONDS)) throw new AssertionError("callback list death timeout");
                        if (list.getRegisteredCallbackCount() != 0) throw new AssertionError("dead callback retained");
                        if (root.isBinderAlive()) throw new AssertionError("dead binder reported alive");
                        if (root.unlinkToDeath(recipient, 0)) throw new AssertionError("unlink after death succeeded");
                        boolean rejected = false;
                        try { root.linkToDeath(recipient, 0); }
                        catch (RemoteException expected) { rejected = true; }
                        if (!rejected) throw new AssertionError("link after death");
                    } finally {
                        request.recycle();
                        reply.recycle();
                    }
                } finally {
                    bridge.close();
                }
                Log.i("BinderGuestTest", "PASS transaction, nested callback, one-way and death");
                runOnUiThread(() -> result.setText("PASS"));
            } catch (Throwable error) {
                Log.e("BinderGuestTest", "FAIL", error);
                runOnUiThread(() -> result.setText("FAIL"));
            }
        }, "binder-test").start();
    }

    private static IBinder boundService(IBinder root, int index) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            if (!root.transact(7, request, reply, 0)) throw new AssertionError("get bound service");
            IBinder router = reply.readStrongBinder();
            if (router == null) throw new AssertionError("missing router");
            request.recycle();
            reply.recycle();
            request = Parcel.obtain();
            reply = Parcel.obtain();
            String action = !isTestReal() ? "megler.test.gmsbroker.BIND" :
                index == 0 ? "com.google.android.gms.ads.identifier.service.START" :
                             "com.google.android.gms.common.service.START";
            request.writeInt(index + 1);
            request.writeString(action);
            request.writeString(null);
            if (!router.transact(SharedMicrogServiceRouter.BIND, request, reply, 0))
                throw new AssertionError("dynamic service bind");
            reply.readException();
            if (reply.readString() == null) throw new AssertionError("missing service component");
            IBinder service = reply.readStrongBinder();
            if (service == null) throw new AssertionError("missing bound service");
            return service;
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void checkRealService(IBinder root) throws Exception {
        IBinder service = boundService(root, 0);
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("com.google.android.gms.ads.identifier.internal.IAdvertisingIdService");
            if (!service.transact(1, request, reply, 0)) throw new AssertionError("advertising ID transaction");
            reply.readException();
            if (!"00000000-0000-0000-0000-000000000000".equals(reply.readString()))
                throw new AssertionError("expected isolated microG default");
            Log.i("BinderGuestTest", "PASS cross-process real microG service");
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void checkRouterLifecycle(IBinder root) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            if (!root.transact(7, request, reply, 0)) throw new AssertionError("router lookup");
            IBinder router = reply.readStrongBinder();
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(1);
            request.writeString(isTestReal() ? "com.google.android.gms.ads.identifier.service.START" :
                                              "megler.test.gmsbroker.BIND");
            request.writeString(null);
            boolean rejected = false;
            try { router.transact(SharedMicrogServiceRouter.BIND, request, reply, 0); }
            catch (RemoteException expected) { rejected = true; }
            if (!rejected) throw new AssertionError("duplicate binding accepted");
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(1);
            if (!router.transact(SharedMicrogServiceRouter.UNBIND, request, reply, 0))
                throw new AssertionError("unbind failed");
            reply.readException();
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(1);
            rejected = false;
            try { router.transact(SharedMicrogServiceRouter.UNBIND, request, reply, 0); }
            catch (RemoteException expected) { rejected = true; }
            if (!rejected) throw new AssertionError("unknown binding unbound");
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(42);
            request.writeString("megler.test.nonexistent.BIND");
            request.writeString(null);
            if (!router.transact(SharedMicrogServiceRouter.BIND, request, reply, 0))
                throw new AssertionError("unknown service request failed");
            reply.readException();
            if (reply.readString() != null || reply.readStrongBinder() != null)
                throw new AssertionError("unknown service bound");
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(42);
            if (!router.transact(SharedMicrogServiceRouter.UNBIND, request, reply, 0))
                throw new AssertionError("unknown service cleanup failed");
            reply.readException();
            if (boundService(root, 0) == null) throw new AssertionError("rebind failed");
            if (!isTestReal()) {
                checkServiceRefused(router, 43, "megler.test.gmsbroker.HIDDEN", null);
                checkServiceRefused(router, 44, null, "megler.test.gmsbroker.Broker$NoFilter");
                checkServiceRefused(router, 45, "megler.test.gmsbroker.PROTECTED", null);
                request.recycle(); reply.recycle();
                request = Parcel.obtain(); reply = Parcel.obtain();
                request.writeInt(2);
                request.writeString(null);
                request.writeString("megler.test.gmsbroker.Broker");
                if (!router.transact(SharedMicrogServiceRouter.BIND, request, reply, 0))
                    throw new AssertionError("component bind failed");
                reply.readException();
                if (!"megler.test.gmsbroker.Broker".equals(reply.readString()) ||
                    reply.readStrongBinder() == null) throw new AssertionError("component bind result");
                request.recycle(); reply.recycle();
                request = Parcel.obtain(); reply = Parcel.obtain();
                request.writeInt(2);
                if (!router.transact(SharedMicrogServiceRouter.UNBIND, request, reply, 0))
                    throw new AssertionError("component unbind failed");
                reply.readException();
            }
            Log.i("BinderGuestTest", "PASS dynamic binding lifecycle");
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void checkServiceRefused(IBinder router, int id, String action, String component) throws Exception {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInt(id);
            request.writeString(action);
            request.writeString(component);
            if (!router.transact(SharedMicrogServiceRouter.BIND, request, reply, 0))
                throw new AssertionError("service request failed");
            reply.readException();
            if (reply.readString() != null || reply.readStrongBinder() != null)
                throw new AssertionError("restricted service bound");
            request.recycle(); reply.recycle();
            request = Parcel.obtain(); reply = Parcel.obtain();
            request.writeInt(id);
            if (!router.transact(SharedMicrogServiceRouter.UNBIND, request, reply, 0))
                throw new AssertionError("service cleanup failed");
            reply.readException();
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void checkRealBroker(IBinder root) throws Exception {
        IBinder broker = boundService(root, 1);
        invokeRealBroker(broker, "megler.test.binderfd", false);
        if (isTestSelfHosted()) {
            invokeRealBroker(broker, "com.example.other", true);
            Log.i("BinderGuestTest", "PASS self-hosted broker rejected another package");
        }
        Log.i("BinderGuestTest", "PASS cross-process real microG broker and callback");
    }

    private static void invokeRealBroker(IBinder broker, String packageName, boolean expectRejection) throws Exception {
        CountDownLatch called = new CountDownLatch(1);
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags)
                    throws RemoteException {
                if (code != 1) return false;
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                if (data.readInt() != 0) throw new AssertionError("broker status");
                IBinder service = data.readStrongBinder();
                if (service == null || !"com.google.android.gms.common.internal.service.ICommonService"
                        .equals(service.getInterfaceDescriptor()))
                    throw new AssertionError("broker service descriptor");
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
            request.writeInt(0xffff0007);
            fieldAt = request.dataPosition(); request.writeInt(0); fieldStart = request.dataPosition();
            request.writeBundle(new Bundle()); end = request.dataPosition();
            request.setDataPosition(fieldAt); request.writeInt(end - fieldStart); request.setDataPosition(end);
            request.writeInt(0xffff000b);
            fieldAt = request.dataPosition(); request.writeInt(0); fieldStart = request.dataPosition();
            request.writeTypedArray(new com.google.android.gms.common.Feature[] {
                new com.google.android.gms.common.Feature("megler_probe", 7)
            }, 0);
            end = request.dataPosition();
            request.setDataPosition(fieldAt); request.writeInt(end - fieldStart); request.setDataPosition(end);
            request.setDataPosition(sizeAt); request.writeInt(end - start); request.setDataPosition(end);
            try {
                if (!broker.transact(46, request, reply, 0)) throw new AssertionError("real broker transaction");
            } catch (RemoteException e) {
                if (expectRejection) return;
                throw e;
            }
            if (expectRejection) throw new AssertionError("broker accepted another package");
            reply.readException();
            if (!called.await(5, TimeUnit.SECONDS)) throw new AssertionError("real broker callback timeout");
        } finally { request.recycle(); reply.recycle(); }
    }

    private static void checkBroker(IBinder root) throws Exception {
        CountDownLatch called = new CountDownLatch(1);
        Binder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel response, int flags) {
                if (code != 1) return false;
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                if (data.readInt() != 0 || !"synthetic-result".equals(data.readString()))
                    throw new AssertionError("broker callback result");
                response.writeNoException();
                called.countDown();
                return true;
            }
        };
        IBinder service = boundService(root, 0);
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("com.google.android.gms.common.internal.IGmsServiceBroker");
            request.writeInt(39);
            request.writeStrongBinder(new Binder());
            request.writeStrongBinder(callback);
            if (!service.transact(46, request, reply, 0)) throw new AssertionError("broker transaction");
            reply.readException();
            if (!called.await(5, TimeUnit.SECONDS)) throw new AssertionError("broker callback timeout");
            Log.i("BinderGuestTest", "PASS cross-process bound service and callback");
        } finally { request.recycle(); reply.recycle(); }
    }
}
