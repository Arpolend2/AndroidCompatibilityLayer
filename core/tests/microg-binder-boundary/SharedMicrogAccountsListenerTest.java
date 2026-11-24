package megler;

import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.content.Intent;
import android.content.ContextWrapper;
import android.os.Bundle;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.io.File;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;

public final class SharedMicrogAccountsListenerTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void worker() throws Exception {
        final class Engine implements SharedMicrogAccounts.Engine {
            volatile int count = 1;
            volatile boolean fail;
            volatile boolean block;
            final AtomicInteger listCalls = new AtomicInteger();
            final CountDownLatch entered = new CountDownLatch(1);
            final CountDownLatch release = new CountDownLatch(1);
            @Override public Account[] accounts() {
                listCalls.incrementAndGet();
                if (block) {
                    entered.countDown();
                    try {
                        if (!release.await(5, TimeUnit.SECONDS)) throw new IllegalStateException("fixture timeout");
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new IllegalStateException(e);
                    }
                }
                if (fail) throw new IllegalStateException("fixture list failure");
                Account[] result = new Account[count];
                for (int i = 0; i < count; ++i)
                    result[i] = new Account(i == 0 ? "fixture.account" : "fixture.second",
                                            SharedMicrogAccounts.GOOGLE);
                return result;
            }
            @Override public AutoCloseable start(int operation, Account account, String tokenType,
                    String[] features, Bundle options, AccountAuthenticatorResponse response) {
                if (operation == SharedMicrogAccounts.OP_ADD) {
                    count = 2;
                    Bundle answer = new Bundle();
                    answer.putString(AccountManager.KEY_ACCOUNT_NAME, "fixture.second");
                    answer.putString(AccountManager.KEY_ACCOUNT_TYPE, SharedMicrogAccounts.GOOGLE);
                    response.onResult(answer);
                }
                return () -> {};
            }
        }
        Engine engine = new Engine();
        SharedMicrogAccounts.Server server = new SharedMicrogAccounts.Server(engine,
            new SharedMicrogAccounts.Foreground() {
                @Override public long launch(long channel, Intent intent) { return 0; }
                @Override public void dismiss(long channel, long generation) {}
            }, 1, "fixture.client", 11001, 50001);
        Binder root = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == SharedMicrogServiceRouter.ACCOUNTS) {
                    reply.writeNoException();
                    reply.writeStrongBinder(server.binder());
                    return true;
                }
                if (code == 100) {
                    int action = data.readInt();
                    if (action >= 0 && action <= 2) engine.count = action;
                    else if (action == 3) engine.fail = true;
                    else if (action == 4) engine.fail = false;
                    else if (action == 5) engine.block = true;
                    else if (action == 6) engine.release.countDown();
                    else throw new RemoteException("Invalid fixture action");
                    reply.writeNoException();
                    return true;
                }
                if (code == 101) {
                    reply.writeNoException();
                    reply.writeInt(engine.listCalls.get());
                    return true;
                }
                if (code == 102) {
                    try {
                        if (!engine.entered.await(5, TimeUnit.SECONDS))
                            throw new RemoteException("Fixture request not entered");
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new RemoteException("Fixture interrupted");
                    }
                    reply.writeNoException();
                    return true;
                }
                return false;
            }
        };
        BinderBridge bridge = new BinderBridge(System.in, System.out, root);
        try { bridge.awaitClosed(); }
        finally { server.close(); }
    }

    private static int control(BinderBridge bridge, int code, int value) throws RemoteException {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            if (code == 100) request.writeInt(value);
            check(bridge.remoteRoot().transact(code, request, reply, 0), "fixture control accepted");
            reply.readException();
            return code == 101 ? reply.readInt() : 0;
        } finally { request.recycle(); reply.recycle(); }
    }

    private static Account[] next(LinkedBlockingQueue<Account[]> queue, String message)
            throws InterruptedException {
        Account[] accounts = queue.poll(6, TimeUnit.SECONDS);
        check(accounts != null, message);
        return accounts;
    }

    private static void waitForSnapshot(int size) throws Exception {
        java.lang.reflect.Field listeners = AccountManager.class.getDeclaredField("sListeners");
        java.lang.reflect.Field snapshot = AccountManager.class.getDeclaredField("sLastSharedAccounts");
        listeners.setAccessible(true);
        snapshot.setAccessible(true);
        Object lock = listeners.get(null);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(6);
        while (System.nanoTime() < deadline) {
            synchronized (lock) {
                Account[] accounts = (Account[]) snapshot.get(null);
                if (accounts != null && accounts.length == size) return;
            }
            Thread.sleep(10);
        }
        throw new AssertionError("fixture snapshot not refreshed");
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "worker".equals(args[0])) { worker(); return; }
        check(!new File(MicroG.SHARED).exists(), "fixture must not access shared account storage");
        File directory = Files.createTempDirectory("megler-listener-fixture").toFile();
        System.setProperty("fixture.dataDir", directory.getAbsolutePath());
        CountDownLatch ready = new CountDownLatch(1);
        Thread main = new Thread(() -> {
            Looper.prepareMainLooper();
            ready.countDown();
            Looper.loop();
        }, "fixture-main");
        main.setDaemon(true);
        main.start();
        check(ready.await(5, TimeUnit.SECONDS), "main looper starts");
        String java = System.getProperty("java.home") + File.separator + "bin" + File.separator + "java";
        Process worker = new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
            SharedMicrogAccountsListenerTest.class.getName(), "worker")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(worker.getInputStream(), worker.getOutputStream(), null);
        SharedMicrogAccounts.Channel channel = SharedMicrogAccounts.install(bridge.remoteRoot());
        SharedMicrogIdentity.startClient(11001, "fixture.client");
        AccountManager manager = new AccountManager(new ContextWrapper(null) {
            @Override public String getOpPackageName() { return "fixture.client"; }
            @Override public String getPackageName() { return "fixture.client"; }
        });
        LinkedBlockingQueue<Account[]> observed = new LinkedBlockingQueue<>();
        android.accounts.OnAccountsUpdateListener listener = observed::add;
        LinkedBlockingQueue<Account[]> changes = new LinkedBlockingQueue<>();
        android.accounts.OnAccountsUpdateListener changeListener = changes::add;
        try {
            check(manager.getAccounts().length == 1, "remote account is available");
            manager.addOnAccountsUpdatedListener(listener, null, true);
            Account[] received = next(observed, "immediate listener callback arrives");
            check(received.length == 1 && "fixture.account".equals(received[0].name),
                  "listener must observe the worker account, not local accounts");

            LinkedBlockingQueue<Account[]> filtered = new LinkedBlockingQueue<>();
            android.accounts.OnAccountsUpdateListener filteredListener = filtered::add;
            manager.addOnAccountsUpdatedListener(filteredListener, null, true, new String[] { "fixture.other" });
            check(next(filtered, "filtered listener callback").length == 0, "account types filter worker list");
            manager.removeOnAccountsUpdatedListener(filteredListener);

            manager.addOnAccountsUpdatedListener(changeListener, null, false);
            check(changes.poll(300, TimeUnit.MILLISECONDS) == null, "non-immediate listener has no initial callback");
            Bundle added = manager.addAccount(SharedMicrogAccounts.GOOGLE, null, null, null, null, null, null)
                                  .getResult(5, TimeUnit.SECONDS);
            check(added != null && "fixture.second".equals(added.getString(AccountManager.KEY_ACCOUNT_NAME)),
                  "real worker add result reaches client");
            check(next(observed, "first listener receives change").length == 2, "first listener sees changed list");
            check(next(changes, "second listener receives change").length == 2, "second listener sees changed list");

            int warnings = Native.accountWarnings.get();
            control(bridge, 100, 3);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(6);
            while (Native.accountWarnings.get() == warnings && System.nanoTime() < deadline)
                Thread.sleep(20);
            check(Native.accountWarnings.get() > warnings, "remote list failure is logged");
            check(observed.isEmpty() && changes.isEmpty(), "remote failure never publishes an empty account list");
            control(bridge, 100, 0);
            control(bridge, 100, 4);
            check(next(observed, "first listener receives real removal").length == 0, "real empty list is delivered");
            check(next(changes, "second listener receives real removal").length == 0, "removal delivered to both");

            CountDownLatch blockedMain = new CountDownLatch(1);
            CountDownLatch unblockMain = new CountDownLatch(1);
            check(new Handler(Looper.getMainLooper()).post(() -> {
                blockedMain.countDown();
                try { unblockMain.await(5, TimeUnit.SECONDS); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            }), "block fixture callback queue");
            check(blockedMain.await(5, TimeUnit.SECONDS), "callback queue is blocked");
            try {
                control(bridge, 100, 1);
                waitForSnapshot(1);
                SharedMicrogAccounts.clear(channel);
            } finally { unblockMain.countDown(); }
            check(observed.poll(300, TimeUnit.MILLISECONDS) == null &&
                  changes.poll(300, TimeUnit.MILLISECONDS) == null,
                  "queued callbacks from a closed channel are discarded");
            int calls = control(bridge, 101, 0);
            Thread.sleep(2300);
            check(control(bridge, 101, 0) == calls && observed.isEmpty() && changes.isEmpty(),
                  "closing channel stops observation without an empty callback");
            channel = SharedMicrogAccounts.install(bridge.remoteRoot());
            check(next(observed, "listener resumes on channel replacement").length == 1,
                  "replacement channel refreshes registered listeners");
            check(next(changes, "second listener resumes on replacement").length == 1,
                  "replacement channel refreshes both listeners");

            manager.removeOnAccountsUpdatedListener(changeListener);
            manager.removeOnAccountsUpdatedListener(listener);
            calls = control(bridge, 101, 0);
            Thread.sleep(2300);
            check(control(bridge, 101, 0) == calls, "removing last listener stops observation");

            LinkedBlockingQueue<Account[]> removed = new LinkedBlockingQueue<>();
            android.accounts.OnAccountsUpdateListener removedListener = removed::add;
            control(bridge, 100, 5);
            manager.addOnAccountsUpdatedListener(removedListener, null, true);
            control(bridge, 102, 0);
            manager.removeOnAccountsUpdatedListener(removedListener);
            control(bridge, 100, 6);
            check(removed.poll(500, TimeUnit.MILLISECONDS) == null,
                  "in-flight refresh cannot notify a removed listener");
        } finally {
            SharedMicrogAccounts.clear(channel);
            bridge.close();
            bridge.awaitClosed();
            check(worker.waitFor(5, TimeUnit.SECONDS), "fixture worker exits");
            Looper.getMainLooper().quit();
            Files.deleteIfExists(directory.toPath());
        }
        System.out.println("SharedMicrogAccountsListenerTest PASS");
    }
}
