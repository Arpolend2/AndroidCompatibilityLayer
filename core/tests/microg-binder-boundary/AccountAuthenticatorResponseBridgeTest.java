import android.accounts.AccountAuthenticatorResponse;
import android.accounts.IAccountAuthenticatorResponse;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import megler.BinderBridge;

public class AccountAuthenticatorResponseBridgeTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void child() throws Exception {
        Binder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != 1) return false;
                AccountAuthenticatorResponse response = new AccountAuthenticatorResponse(data);
                response.onRequestContinued();
                Bundle answer = new Bundle();
                answer.putString("fixture", "response");
                response.onResult(answer);
                response.onResult(null);
                response.onError(7, "fixture-error");
                reply.writeNoException();
                return true;
            }
        };
        BinderBridge bridge = new BinderBridge(System.in, System.out, service);
        bridge.awaitClosed();
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "child".equals(args[0])) { child(); return; }
        CountDownLatch received = new CountDownLatch(4);
        AtomicInteger results = new AtomicInteger(), nullResults = new AtomicInteger();
        AtomicInteger continuations = new AtomicInteger(), errors = new AtomicInteger();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        IAccountAuthenticatorResponse.Stub callback = new IAccountAuthenticatorResponse.Stub() {
            @Override public void onResult(Bundle value) {
                try {
                    if (value == null) nullResults.incrementAndGet();
                    else {
                        check("response".equals(value.getString("fixture")), "result payload");
                        results.incrementAndGet();
                    }
                } catch (Throwable t) { failure.set(t); }
                finally { received.countDown(); }
            }
            @Override public void onRequestContinued() { continuations.incrementAndGet(); received.countDown(); }
            @Override public void onError(int code, String message) {
                try {
                    check(code == 7 && "fixture-error".equals(message), "error payload");
                    errors.incrementAndGet();
                }
                catch (Throwable t) { failure.set(t); }
                finally { received.countDown(); }
            }
        };
        check(IAccountAuthenticatorResponse.Stub.asInterface(callback) == callback, "local interface");
        Parcel invalid = Parcel.obtain();
        try {
            invalid.writeInterfaceToken("not-the-interface");
            invalid.writeTypedObject(null, 0);
            boolean rejected = false;
            try { callback.transact(IBinder.FIRST_CALL_TRANSACTION, invalid, null, IBinder.FLAG_ONEWAY); }
            catch (SecurityException expected) { rejected = true; }
            check(rejected, "wrong interface rejected");
        } finally { invalid.recycle(); }

        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin"
            + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                AccountAuthenticatorResponseBridgeTest.class.getName(), "child")
                .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        try {
            Parcel request = Parcel.obtain(), reply = Parcel.obtain();
            try {
                new AccountAuthenticatorResponse(callback).writeToParcel(request, 0);
                check(bridge.remoteRoot().transact(1, request, reply, 0), "remote transaction");
                reply.readException();
            } finally { request.recycle(); reply.recycle(); }
            check(received.await(10, TimeUnit.SECONDS), "callbacks delivered across processes");
            if (failure.get() != null) throw new AssertionError("callback failed", failure.get());
            check(results.get() == 1 && nullResults.get() == 1 && continuations.get() == 1 && errors.get() == 1,
                    "each callback delivered exactly once");
        } finally {
            bridge.close();
            bridge.awaitClosed();
            check(child.waitFor(10, TimeUnit.SECONDS), "child stopped");
            check(child.exitValue() == 0, "child exit");
        }

        Binder reject = new Binder();
        IAccountAuthenticatorResponse remote = IAccountAuthenticatorResponse.Stub.asInterface(reject);
        boolean failed = false;
        try { remote.onRequestContinued(); } catch (RemoteException expected) { failed = true; }
        check(failed, "rejected transaction reported");
        System.out.println("AccountAuthenticatorResponseBridgeTest PASS");
    }
}
