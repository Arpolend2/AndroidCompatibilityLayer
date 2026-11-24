import android.accounts.AbstractAccountAuthenticator;
import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.IAccountAuthenticatorResponse;
import android.accounts.NetworkErrorException;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import megler.BinderBridge;

public class AccountAuthenticatorTransportTest {
    private static final String DESCRIPTOR = "android.accounts.IAccountAuthenticator";

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Authenticator extends AbstractAccountAuthenticator {
        Authenticator() { super(null); }
        @Override public Bundle addAccount(AccountAuthenticatorResponse response, String type, String tokenType,
                                            String[] features, Bundle options) {
            check("fixture.type".equals(type) && "fixture.scope".equals(tokenType), "add arguments");
            check(features.length == 1 && "fixture.feature".equals(features[0]), "features");
            check(options.getBoolean("fixture.option"), "options");
            Bundle result = new Bundle();
            result.putBoolean("fixture.added", true);
            return result;
        }
        @Override public Bundle confirmCredentials(AccountAuthenticatorResponse response, Account account, Bundle options) {
            check("fixture.account".equals(account.name) && "fixture.type".equals(account.type), "account");
            check(options == null, "null options");
            Bundle result = new Bundle();
            result.putBoolean("fixture.confirmed", true);
            response.onResult(result);
            return null;
        }
        @Override public Bundle getAuthToken(AccountAuthenticatorResponse response, Account account, String type,
                                             Bundle options) throws NetworkErrorException {
            throw new NetworkErrorException("private fixture detail");
        }
        @Override public String getAuthTokenLabel(String type) { return type; }
        @Override public Bundle editProperties(AccountAuthenticatorResponse response, String type) {
            Bundle result = new Bundle();
            result.putStringArray("fixture.unsupported", new String[] { "unsupported" });
            return result;
        }
        @Override public Bundle updateCredentials(AccountAuthenticatorResponse response, Account account, String type,
                                                  Bundle options) { return null; }
        @Override public Bundle hasFeatures(AccountAuthenticatorResponse response, Account account, String[] features) { return null; }
    }

    private static void child() throws Exception {
        BinderBridge bridge = new BinderBridge(System.in, System.out, new Authenticator().getIBinder());
        bridge.awaitClosed();
    }

    private static Parcel request(IAccountAuthenticatorResponse response) {
        Parcel data = Parcel.obtain();
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeStrongBinder(response.asBinder());
        return data;
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "child".equals(args[0])) { child(); return; }
        Authenticator local = new Authenticator();
        check(AbstractAccountAuthenticator.of(local.getIBinder()) == local, "local transport retained");
        Parcel bad = Parcel.obtain();
        try {
            bad.writeInterfaceToken("wrong");
            boolean rejected = false;
            try { local.getIBinder().transact(1, bad, null, IBinder.FLAG_ONEWAY); }
            catch (SecurityException expected) { rejected = true; }
            check(rejected, "wrong interface rejected");
        } finally { bad.recycle(); }

        CountDownLatch received = new CountDownLatch(4);
        AtomicInteger results = new AtomicInteger(), errors = new AtomicInteger(), invalid = new AtomicInteger();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        IAccountAuthenticatorResponse response = new IAccountAuthenticatorResponse.Stub() {
            @Override public void onResult(Bundle result) {
                try {
                    check(result.getBoolean("fixture.added") || result.getBoolean("fixture.confirmed"), "result");
                    results.incrementAndGet();
                } catch (Throwable t) { failure.set(t); }
                finally { received.countDown(); }
            }
            @Override public void onRequestContinued() { }
            @Override public void onError(int code, String message) {
                try {
                    if (code == AccountManager.ERROR_CODE_NETWORK_ERROR) {
                        check("Authenticator network error".equals(message), "network error message");
                        errors.incrementAndGet();
                    } else {
                        check(code == AccountManager.ERROR_CODE_INVALID_RESPONSE, "error code");
                        check("Authenticator result could not be delivered".equals(message), "delivery error message");
                        invalid.incrementAndGet();
                    }
                } catch (Throwable t) { failure.set(t); }
                finally { received.countDown(); }
            }
        };
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin" + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
            AccountAuthenticatorTransportTest.class.getName(), "child").redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        try {
            IBinder remote = bridge.remoteRoot();
            Parcel data = request(response);
            try {
                data.writeString("fixture.type");
                data.writeString("fixture.scope");
                data.writeStringArray(new String[] { "fixture.feature" });
                Bundle options = new Bundle();
                options.putBoolean("fixture.option", true);
                data.writeInt(1);
                options.writeToParcel(data, 0);
                check(remote.transact(1, data, null, IBinder.FLAG_ONEWAY), "add transaction");
            } finally { data.recycle(); }
            data = request(response);
            try {
                data.writeInt(1);
                new Account("fixture.account", "fixture.type").writeToParcel(data, 0);
                data.writeInt(0);
                check(remote.transact(2, data, null, IBinder.FLAG_ONEWAY), "confirm transaction");
            } finally { data.recycle(); }
            data = request(response);
            try {
                data.writeInt(1);
                new Account("fixture.account", "fixture.type").writeToParcel(data, 0);
                data.writeString("fixture.scope");
                data.writeInt(0);
                check(remote.transact(3, data, null, IBinder.FLAG_ONEWAY), "token request transaction");
            } finally { data.recycle(); }
            data = request(response);
            try {
                data.writeString("fixture.type");
                check(remote.transact(6, data, null, IBinder.FLAG_ONEWAY), "unsupported response transaction");
            } finally { data.recycle(); }
            check(received.await(10, TimeUnit.SECONDS), "callbacks delivered");
            if (failure.get() != null) throw new AssertionError("callback failed", failure.get());
            check(results.get() == 2 && errors.get() == 1 && invalid.get() == 1, "callback counts");
        } finally {
            bridge.close();
            bridge.awaitClosed();
            check(child.waitFor(10, TimeUnit.SECONDS), "child stopped");
            check(child.exitValue() == 0, "child exit");
        }
        System.out.println("AccountAuthenticatorTransportTest PASS");
    }
}
