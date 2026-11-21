import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public class BinderCallerTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        CountDownLatch entered = new CountDownLatch(2);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Binder target = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                int uid = Binder.getCallingUid(), pid = Binder.getCallingPid();
                entered.countDown();
                try {
                    if (!entered.await(5, TimeUnit.SECONDS)) throw new AssertionError("parallel calls");
                } catch (InterruptedException e) {
                    throw new RemoteException("interrupted");
                }
                Binder nested = new Binder() {
                    @Override protected boolean onTransact(int c, Parcel d, Parcel r, int f) {
                        check(Binder.getCallingUid() == 17000 && Binder.getCallingPid() == 9000, "nested caller");
                        throw new IllegalStateException("synthetic failure");
                    }
                };
                try { Binder.meglerTransactWithCaller(nested, 1, data, reply, 0, 17000, 9000); }
                catch (IllegalStateException expected) {}
                check(Binder.getCallingUid() == uid && Binder.getCallingPid() == pid, "outer caller restored");
                check(uid == code && pid == code + 100, "parallel caller isolated");
                return true;
            }
        };
        Runnable makeCall = () -> {
            int uid = Thread.currentThread().getName().equals("first") ? 14001 : 14002;
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                Binder.meglerTransactWithCaller(target, uid, data, reply, 0, uid, uid + 100);
                check(Binder.getCallingUid() == android.os.Process.myUid() &&
                      Binder.getCallingPid() == android.os.Process.myPid(), "caller cleared after dispatch");
            } catch (Throwable e) { failure.compareAndSet(null, e); }
            finally { data.recycle(); reply.recycle(); }
        };
        Thread first = new Thread(makeCall, "first"), second = new Thread(makeCall, "second");
        first.start();
        second.start();
        first.join(10000);
        second.join(10000);
        check(!first.isAlive() && !second.isAlive() && failure.get() == null, "concurrent dispatch: " + failure.get());
        System.out.println("Binder caller identity isolation tests passed");
    }
}
