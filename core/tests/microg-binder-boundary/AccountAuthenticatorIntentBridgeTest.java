import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.IAccountAuthenticatorResponse;
import android.content.ComponentName;
import android.content.Intent;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import megler.BinderBridge;

public class AccountAuthenticatorIntentBridgeTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void child() throws Exception {
        Binder root = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code != 1) return false;
                Bundle result = data.readTypedObject(Bundle.CREATOR);
                Intent intent = result.getParcelable("intent");
                check("fixture.action".equals(intent.getAction()), "action");
                check("fixture:content".equals(intent.getDataString()), "data");
                check("fixture/type".equals(intent.getType()), "type");
                check("fixture.package".equals(intent.getPackage()), "package");
                check(new ComponentName("fixture.package", "fixture.package.Screen").equals(intent.getComponent()), "component");
                check("fixture.id".equals(intent.getIdentifier()) && intent.getFlags() == 16, "identifier and flags");
                check(intent.hasCategory("fixture.category"), "category");
                Account account = intent.getParcelableExtra("account");
                check("fixture.account".equals(account.name) && "fixture.type".equals(account.type), "account");
                AccountAuthenticatorResponse callback = intent.getParcelableExtra("response");
                Bundle answer = new Bundle();
                answer.putBoolean("fixture.finished", true);
                callback.onResult(answer);
                reply.writeNoException();
                return true;
            }
        };
        BinderBridge bridge = new BinderBridge(System.in, System.out, root);
        bridge.awaitClosed();
    }

    private static void rejectMalformedIntent() {
        Binder binder = new Binder();
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeStrongBinder(binder);
            parcel.writeValue(new Intent());
            byte[] wire = parcel.meglerMarshallBinders(b -> 1);
            java.nio.ByteBuffer bytes = java.nio.ByteBuffer.wrap(wire);
            int objectStart = 12 + bytes.getInt(4);
            int intentTag = objectStart + 4 + 1 + 8 + 4;
            check(wire[intentTag] == 10, "intent wire tag");
            bytes.putInt(intentTag + 1 + 7 + 4, -1);
            int[] imports = { 0 };
            boolean rejected = false;
            try { Parcel.meglerUnmarshallBinders(wire, h -> { imports[0]++; return binder; }); }
            catch (IllegalArgumentException expected) { rejected = true; }
            check(rejected && imports[0] == 0, "malformed intent rejected before handle import");
        } finally { parcel.recycle(); }
    }

    private static void rejectIntent(IBinder remote, Intent intent) throws Exception {
        Bundle bad = new Bundle();
        bad.putParcelable("intent", intent);
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeTypedObject(bad, 0);
            boolean rejected = false;
            try { remote.transact(1, data, reply, 0); }
            catch (RemoteException expected) { rejected = true; }
            check(rejected, "unsupported Intent rejected");
        } finally { data.recycle(); reply.recycle(); }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "child".equals(args[0])) { child(); return; }
        rejectMalformedIntent();
        CountDownLatch delivered = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        IAccountAuthenticatorResponse callback = new IAccountAuthenticatorResponse.Stub() {
            @Override public void onResult(Bundle result) {
                try { check(result.getBoolean("fixture.finished"), "callback result"); }
                catch (Throwable t) { failure.set(t); }
                finally { delivered.countDown(); }
            }
            @Override public void onRequestContinued() { }
            @Override public void onError(int code, String message) {
                failure.set(new AssertionError("unexpected callback error"));
                delivered.countDown();
            }
        };
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin" + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
            AccountAuthenticatorIntentBridgeTest.class.getName(), "child").redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        IBinder remote = bridge.remoteRoot();
        try {
            Intent unsupported = new Intent("fixture.action");
            unsupported.setSelector(new Intent("fixture.selector"));
            rejectIntent(remote, unsupported);
            Intent unsupportedExtras = new Intent("fixture.action");
            unsupportedExtras.putExtra("array", new String[] { "unsupported" });
            rejectIntent(remote, unsupportedExtras);

            Intent intent = new Intent("fixture.action");
            intent.setDataAndType(Uri.parse("fixture:content"), "fixture/type");
            intent.setPackage("fixture.package");
            intent.setComponent(new ComponentName("fixture.package", "fixture.package.Screen"));
            intent.setIdentifier("fixture.id");
            intent.setFlags(16);
            intent.addCategory("fixture.category");
            intent.putExtra("account", new Account("fixture.account", "fixture.type"));
            intent.putExtra("response", new AccountAuthenticatorResponse(callback));
            Bundle result = new Bundle();
            result.putParcelable("intent", intent);
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeTypedObject(result, 0);
                check(remote.transact(1, data, reply, 0), "remote transaction");
                reply.readException();
            } finally { data.recycle(); reply.recycle(); }
            check(delivered.await(10, TimeUnit.SECONDS), "callback delivered");
            if (failure.get() != null) throw new AssertionError("callback failed", failure.get());
        } finally {
            bridge.close();
            bridge.awaitClosed();
            check(child.waitFor(10, TimeUnit.SECONDS), "child stopped");
            check(child.exitValue() == 0, "child exit");
        }
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            boolean failed = false;
            try { failed = !remote.transact(1, data, reply, 0); }
            catch (RemoteException expected) { failed = true; }
            check(failed, "closed bridge rejects request");
        } finally { data.recycle(); reply.recycle(); }
        System.out.println("AccountAuthenticatorIntentBridgeTest PASS");
    }
}
