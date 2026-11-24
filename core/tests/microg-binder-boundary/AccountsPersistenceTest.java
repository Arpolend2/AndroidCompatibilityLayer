package megler;

import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.IAccountAuthenticatorResponse;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;
import java.io.File;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class AccountsPersistenceTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Callback extends IAccountAuthenticatorResponse.Stub {
        final CountDownLatch finished = new CountDownLatch(1);
        @Override public void onResult(Bundle result) { finished.countDown(); }
        @Override public void onRequestContinued() {}
        @Override public void onError(int code, String message) { finished.countDown(); }
        void await() throws InterruptedException {
            check(finished.await(5, TimeUnit.SECONDS), "fixture outcome callback");
        }
    }

    private static void testOutcomeMarkers() throws Exception {
        PrintStream previous = System.err;
        ByteArrayOutputStream captured = new ByteArrayOutputStream();
        PrintStream log = new PrintStream(captured, true, "UTF-8");
        SharedMicrogAccounts.Server server = new SharedMicrogAccounts.Server(new SharedMicrogAccounts.Engine() {
            @Override public Account[] accounts() {
                return new Account[] { new Account("fixture.account", SharedMicrogAccounts.GOOGLE) };
            }
            @Override public AutoCloseable start(int operation, Account account, String tokenType,
                    String[] features, Bundle options, AccountAuthenticatorResponse response) {
                String mode = options.getString("fixture.mode");
                if ("validated".equals(mode)) {
                    Bundle answer = new Bundle();
                    answer.putString(AccountManager.KEY_ACCOUNT_NAME, "fixture.account");
                    answer.putString(AccountManager.KEY_ACCOUNT_TYPE, SharedMicrogAccounts.GOOGLE);
                    answer.putString(AccountManager.KEY_AUTHTOKEN, "fixture-token");
                    response.onResult(answer);
                } else if ("error".equals(mode)) {
                    response.onError(AccountManager.ERROR_CODE_NETWORK_ERROR, "fixture-secret");
                }
                return () -> {};
            }
        }, new SharedMicrogAccounts.Foreground() {
            @Override public boolean launch(long channel, Intent intent) { return false; }
            @Override public void dismiss(long channel) {}
        }, 1, "fixture.client", 11001, 50001);
        SharedMicrogAccounts.Client client = new SharedMicrogAccounts.Client(server.binder());
        try {
            System.setErr(log);
            Bundle options = new Bundle();
            options.putString("fixture.mode", "validated");
            Callback validated = new Callback();
            client.add(null, null, options, validated);
            validated.await();
            options.putString("fixture.mode", "error");
            Callback error = new Callback();
            client.add(null, null, options, error);
            error.await();
            options.putString("fixture.mode", "delayed");
            Callback canceled = new Callback();
            int id = client.add(null, null, options, canceled);
            client.cancel(id);
            canceled.await();
            Callback timedOut = new Callback();
            id = client.add(null, null, options, timedOut);
            Field field = SharedMicrogAccounts.Server.class.getDeclaredField("pending");
            field.setAccessible(true);
            Map<?, ?> pending = (Map<?, ?>) field.get(server);
            Object request;
            synchronized (pending) { request = pending.get(id); }
            check(request != null, "pending timeout fixture");
            Method timeout = request.getClass().getDeclaredMethod("timedOut");
            timeout.setAccessible(true);
            timeout.invoke(request);
            timedOut.await();
            Callback closed = new Callback();
            client.add(null, null, options, closed);
            server.close();
            closed.await();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            String text;
            do {
                text = captured.toString("UTF-8");
                if (text.split("callback submitted", -1).length - 1 == 5) break;
                Thread.sleep(5);
            } while (System.nanoTime() < deadline);
            for (String outcome : new String[] { "VALIDATED", "ERROR", "CANCELED", "TIMEOUT", "CHANNEL_CLOSED" })
                check(text.contains("operation=2 outcome=" + outcome), "distinct fixed outcome marker");
            check(text.split("callback submitted", -1).length - 1 == 5,
                  "callbacks are marked as submitted, not client-confirmed");
            check(!text.contains("fixture.account") && !text.contains("fixture-token") &&
                  !text.contains("fixture-secret") && !text.contains("fixture.client"),
                  "outcome markers contain no account, token or client data");
        } finally {
            server.close();
            client.close();
            System.setErr(previous);
            log.close();
        }
    }

    public static void main(String[] args) throws Exception {
        check(!new File(MicroG.SHARED).exists(), "fixture must not access shared account storage");
        if (args.length == 2 && "serve".equals(args[0])) {
            System.setProperty("fixture.dataDir", args[1]);
            Looper.prepareMainLooper();
            SharedMicrogAccounts.Engine engine = new SharedMicrogAccounts.Engine() {
                @Override public Account[] accounts() { return Accounts.accounts(SharedMicrogAccounts.GOOGLE); }
                @Override public AutoCloseable start(int operation, Account account, String tokenType,
                        String[] features, Bundle options, AccountAuthenticatorResponse response) {
                    throw new UnsupportedOperationException();
                }
            };
            SharedMicrogAccounts.Server server = new SharedMicrogAccounts.Server(engine,
                new SharedMicrogAccounts.Foreground() {
                    @Override public boolean launch(long channel, Intent intent) { return false; }
                    @Override public void dismiss(long channel) {}
                }, 1, "fixture.client", 11001, 50001);
            BinderBridge bridge = new BinderBridge(System.in, System.out, server.binder());
            try { bridge.awaitClosed(); }
            finally { server.close(); }
            return;
        }
        if (args.length == 2 && "check".equals(args[0])) {
            System.setProperty("fixture.dataDir", args[1]);
            Looper.prepareMainLooper();
            Account account = new Account("fixture.account", SharedMicrogAccounts.GOOGLE);
            check(Accounts.exists(account) && "fixture-password".equals(Accounts.password(account)),
                  "successful account survives worker restart");
            return;
        }
        if (args.length == 2 && ("tokens".equals(args[0]) || "tokens-check".equals(args[0]))) {
            System.setProperty("fixture.dataDir", args[1]);
            Looper.prepareMainLooper();
            Account account = new Account("fixture.account", SharedMicrogAccounts.GOOGLE);
            check("fixture-token".equals(Accounts.token(account, "fixture-scope")), "fixture token exists");
            if ("tokens".equals(args[0])) {
                File blocker = new File(args[1], "accounts" + File.separator + "accounts.json.new");
                check(blocker.mkdir(), "block fixture token invalidation");
                boolean failed = false;
                try { Accounts.invalidateToken(SharedMicrogAccounts.GOOGLE, "fixture-token"); }
                catch (IllegalStateException expected) { failed = true; }
                check(failed && "fixture-token".equals(Accounts.token(account, "fixture-scope")),
                      "failed token invalidation is reported and rolled back");
            }
            return;
        }
        File directory = Files.createTempDirectory("megler-account-fixture").toFile();
        File accountsDirectory = new File(directory, "accounts");
        File accountsFile = new File(accountsDirectory, "accounts.json");
        File blockedWrite = new File(accountsDirectory, "accounts.json.new");
        File tokenDirectory = new File(directory, "token-data");
        File tokenAccounts = new File(tokenDirectory, "accounts");
        File tokenFile = new File(tokenAccounts, "accounts.json");
        try {
            System.setProperty("fixture.dataDir", directory.getAbsolutePath());
            check(accountsDirectory.createNewFile(), "block fixture accounts directory");
            Looper.prepareMainLooper();
            Account account = new Account("fixture.account", SharedMicrogAccounts.GOOGLE);
            check(!Accounts.add(account, null, null), "failed persistence must reject add");
            check(Accounts.accounts(SharedMicrogAccounts.GOOGLE).length == 0,
                  "failed persistence must not publish in-memory account");
            check(accountsDirectory.delete() && accountsDirectory.mkdir(), "unblock fixture directory");
            Bundle userData = new Bundle();
            userData.putString("fixture-key", "fixture-value");
            check(Accounts.add(account, "fixture-password", userData), "fixture account is persisted");
            check(accountsFile.isFile(), "fixture account file exists");
            check(blockedWrite.mkdir(), "block fixture replacement");
            boolean failed = false;
            try { Accounts.setPassword(account, "fixture-new-password"); }
            catch (IllegalStateException expected) { failed = true; }
            check(failed && "fixture-password".equals(Accounts.password(account)),
                  "failed password write is reported and rolled back");
            check(blockedWrite.mkdir(), "block fixture user data");
            failed = false;
            try { Accounts.setUserData(account, "fixture-key", "fixture-other"); }
            catch (IllegalStateException expected) { failed = true; }
            check(failed && "fixture-value".equals(Accounts.userData(account, "fixture-key")),
                  "failed user data write is reported and rolled back");
            check(blockedWrite.mkdir(), "block fixture token");
            failed = false;
            try { Accounts.setToken(account, "fixture-scope", "fixture-token"); }
            catch (IllegalStateException expected) { failed = true; }
            check(failed && Accounts.token(account, "fixture-scope") == null,
                  "failed token write is reported and rolled back");
            check(blockedWrite.mkdir(), "block fixture rename");
            check(Accounts.rename(account, "fixture.renamed") == null && Accounts.exists(account),
                  "failed rename is reported and rolled back");
            check(blockedWrite.mkdir(), "block fixture removal");
            check(!Accounts.remove(account) && Accounts.exists(account),
                  "failed removal is reported and rolled back");
            String java = System.getProperty("java.home") + File.separator + "bin" + File.separator + "java";
            Process child = new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                AccountsPersistenceTest.class.getName(), "check", directory.getAbsolutePath())
                .inheritIO().start();
            check(child.waitFor(5, TimeUnit.SECONDS) && child.exitValue() == 0,
                  "persisted fixture survives separate process");
            child = new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                AccountsPersistenceTest.class.getName(), "serve", directory.getAbsolutePath())
                .redirectError(ProcessBuilder.Redirect.INHERIT).start();
            BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
            SharedMicrogAccounts.Client client = new SharedMicrogAccounts.Client(bridge.remoteRoot());
            try {
                Account[] remote = client.accounts();
                check(remote.length == 1 && account.equals(remote[0]),
                      "persisted worker account is available through restricted Binder list");
            } finally {
                client.close();
                bridge.close();
                bridge.awaitClosed();
                check(child.waitFor(5, TimeUnit.SECONDS) && child.exitValue() == 0,
                      "fixture account server exits");
            }
            check(tokenAccounts.mkdirs(), "create isolated token fixture");
            String fixture = "{\"accounts\":[{\"name\":\"fixture.account\",\"type\":\"com.google\","
                + "\"tokens\":{\"fixture-scope\":\"fixture-token\"}}]}";
            Files.write(tokenFile.toPath(), fixture.getBytes(StandardCharsets.UTF_8));
            for (String mode : new String[] { "tokens", "tokens-check" }) {
                child = new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                    AccountsPersistenceTest.class.getName(), mode, tokenDirectory.getAbsolutePath())
                    .inheritIO().start();
                check(child.waitFor(5, TimeUnit.SECONDS) && child.exitValue() == 0,
                      "token fixture persists after failed invalidation");
            }
            testOutcomeMarkers();
        } finally {
            Files.deleteIfExists(new File(tokenAccounts, "accounts.json.new").toPath());
            Files.deleteIfExists(tokenFile.toPath());
            Files.deleteIfExists(tokenAccounts.toPath());
            Files.deleteIfExists(tokenDirectory.toPath());
            Files.deleteIfExists(blockedWrite.toPath());
            Files.deleteIfExists(accountsFile.toPath());
            Files.deleteIfExists(accountsDirectory.toPath());
            Files.deleteIfExists(directory.toPath());
        }
        System.out.println("AccountsPersistenceTest PASS");
    }
}
