package megler;

import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.IAccountAuthenticatorResponse;
import android.content.Intent;
import android.os.Bundle;
import android.os.RemoteException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class SharedMicrogAccountsTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Callback extends IAccountAuthenticatorResponse.Stub {
        final CountDownLatch done = new CountDownLatch(1);
        volatile Bundle result;
        volatile int error;
        @Override public void onResult(Bundle result) { this.result = result; done.countDown(); }
        @Override public void onRequestContinued() { }
        @Override public void onError(int code, String message) { error = code; done.countDown(); }
        void await() throws Exception { check(done.await(10, TimeUnit.SECONDS), "account callback timed out"); }
    }

    private static void child() throws Exception {
        AtomicBoolean screenLaunched = new AtomicBoolean();
        AtomicBoolean screenDismissed = new AtomicBoolean();
        SharedMicrogAccounts.Engine engine = new SharedMicrogAccounts.Engine() {
            @Override public Account[] accounts() {
                return new Account[] { new Account("fixture.account", SharedMicrogAccounts.GOOGLE) };
            }
            @Override public AutoCloseable start(int operation, Account account, String tokenType, String[] features,
                                                  Bundle options, AccountAuthenticatorResponse response) {
                check("fixture.client".equals(options.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME)), "channel package");
                check(options.getInt(AccountManager.KEY_CALLER_UID) == 11001, "channel uid");
                check(options.getInt(AccountManager.KEY_CALLER_PID) == 50001, "channel pid");
                String mode = options.getString("fixture.mode");
                if ("delayed".equals(mode)) return () -> { };
                Bundle answer = new Bundle();
                if (operation == SharedMicrogAccounts.OP_FEATURES) {
                    answer.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
                } else if ("screen".equals(mode)) {
                    answer.putParcelable(AccountManager.KEY_INTENT,
                        new Intent().setClassName("com.google.android.gms", "fixture.Login"));
                    response.onResult(answer);
                    Thread completion = new Thread(() -> {
                        try { Thread.sleep(100); }
                        catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
                        Bundle result = new Bundle();
                        result.putString(AccountManager.KEY_ACCOUNT_NAME, "fixture.account");
                        result.putString(AccountManager.KEY_ACCOUNT_TYPE, SharedMicrogAccounts.GOOGLE);
                        result.putString(AccountManager.KEY_AUTHTOKEN, "test-only-value");
                        response.onResult(result);
                    });
                    completion.setDaemon(true);
                    completion.start();
                    return () -> { };
                } else if ("screen-rejected".equals(mode)) {
                    answer.putParcelable(AccountManager.KEY_INTENT,
                        new Intent().setClassName("com.google.android.gms", "fixture.NotLogin"));
                } else {
                    answer.putString(AccountManager.KEY_ACCOUNT_NAME,
                        "wrong".equals(mode) ? "fixture.other" : "fixture.account");
                    answer.putString(AccountManager.KEY_ACCOUNT_TYPE, SharedMicrogAccounts.GOOGLE);
                    answer.putString(AccountManager.KEY_AUTHTOKEN, "test-only-value");
                    answer.putString(AccountManager.KEY_PASSWORD, "test-only-private-value");
                }
                response.onResult(answer);
                return () -> { };
            }
        };
        SharedMicrogAccounts.Server server = new SharedMicrogAccounts.Server(engine,
            new SharedMicrogAccounts.Foreground() {
                @Override public boolean launch(long channelId, Intent intent) {
                    if (channelId != 1 || intent.getComponent() == null ||
                            !"com.google.android.gms".equals(intent.getComponent().getPackageName()) ||
                            !"fixture.Login".equals(intent.getComponent().getClassName()))
                        return false;
                    screenLaunched.set(true);
                    return true;
                }
                @Override public void dismiss(long channelId) {
                    if (channelId == 1) screenDismissed.set(true);
                }
            }, 1, "fixture.client", 11001, 50001);
        BinderBridge bridge = new BinderBridge(System.in, System.out, server.binder());
        try { bridge.awaitClosed(); }
        finally {
            server.close();
            check(screenLaunched.get(), "trusted sign-in screen launched in worker");
            check(screenDismissed.get(), "trusted sign-in screen dismissed after authenticator result");
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "child".equals(args[0])) { child(); return; }
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin" + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
            SharedMicrogAccountsTest.class.getName(), "child").redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        SharedMicrogAccounts.Client client = new SharedMicrogAccounts.Client(bridge.remoteRoot());
        try {
            Account[] accounts = client.accounts();
            check(accounts.length == 1 && "fixture.account".equals(accounts[0].name), "account list");
            Bundle options = new Bundle();
            options.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME, "spoofed");
            options.putInt(AccountManager.KEY_CALLER_UID, 9);
            options.putInt(AccountManager.KEY_CALLER_PID, 9);
            Callback add = new Callback();
            client.add(null, null, options, add);
            add.await();
            check(add.result != null && add.result.size() == 2 &&
                !add.result.containsKey(AccountManager.KEY_PASSWORD) &&
                !add.result.containsKey(AccountManager.KEY_AUTHTOKEN), "add result is restricted");

            Callback token = new Callback();
            client.token(accounts[0], "fixture.scope", options, token);
            token.await();
            check(token.result != null && token.result.size() == 3 &&
                "test-only-value".equals(token.result.getString(AccountManager.KEY_AUTHTOKEN)) &&
                !token.result.containsKey(AccountManager.KEY_PASSWORD), "token result is restricted");

            Callback features = new Callback();
            client.hasFeatures(accounts[0], new String[] { "fixture.feature" }, options, features);
            features.await();
            check(features.result != null && features.result.size() == 1 &&
                features.result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT), "features result is restricted");

            options.putString("fixture.mode", "wrong");
            Callback mismatch = new Callback();
            client.token(accounts[0], "fixture.scope", options, mismatch);
            mismatch.await();
            check(mismatch.result == null && mismatch.error == AccountManager.ERROR_CODE_INVALID_RESPONSE, "wrong account rejected");

            options.putString("fixture.mode", "screen");
            Callback screen = new Callback();
            client.token(accounts[0], "fixture.scope", options, screen);
            screen.await();
            check(screen.result != null && screen.error == 0 &&
                "test-only-value".equals(screen.result.getString(AccountManager.KEY_AUTHTOKEN)) &&
                !screen.result.containsKey(AccountManager.KEY_INTENT) &&
                !screen.result.containsKey(AccountManager.KEY_PASSWORD),
                "sign-in screen stays private and only the authenticator token returns");

            options.putString("fixture.mode", "screen-rejected");
            Callback rejectedScreen = new Callback();
            client.add(null, null, options, rejectedScreen);
            rejectedScreen.await();
            check(rejectedScreen.result == null &&
                rejectedScreen.error == AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                "untrusted sign-in activity rejected");

            options.putString("fixture.mode", "delayed");
            Callback canceled = new Callback();
            int id = client.add(null, null, options, canceled);
            client.cancel(id);
            canceled.await();
            check(canceled.result == null && canceled.error == AccountManager.ERROR_CODE_CANCELED, "cancellation");

            Bundle bad = new Bundle();
            bad.putString(AccountManager.KEY_PASSWORD, "fixture-only-secret");
            boolean rejected = false;
            try { client.add(null, null, bad, new Callback()); }
            catch (RemoteException | RuntimeException expected) { rejected = true; }
            check(rejected, "password options rejected");
            bad = new Bundle();
            bad.putBundle("nested", new Bundle());
            rejected = false;
            try { client.add(null, null, bad, new Callback()); }
            catch (RemoteException | RuntimeException expected) { rejected = true; }
            check(rejected, "nested options rejected");

            Callback disconnected = new Callback();
            client.add(null, null, options, disconnected);
            bridge.close();
            disconnected.await();
            check(disconnected.result == null && disconnected.error == AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "disconnect");
        } finally {
            client.close();
            bridge.close();
            bridge.awaitClosed();
            check(child.waitFor(10, TimeUnit.SECONDS), "child stopped");
            check(child.exitValue() == 0, "child exit");
        }
        System.out.println("SharedMicrogAccountsTest PASS");
    }
}
