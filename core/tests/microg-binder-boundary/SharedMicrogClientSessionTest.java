package megler;

import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import java.io.Closeable;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

public final class SharedMicrogClientSessionTest {
    private static final String PACKAGE = "com.google.android.gms";
    private static final String ACTION = "com.google.android.gms.common.service.START";

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void await(CountDownLatch latch, String message) throws InterruptedException {
        check(latch.await(5, TimeUnit.SECONDS), message);
    }

    private static final class ServiceContext extends ContextWrapper {
        int binds;
        int unbinds;
        final CountDownLatch entered = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);
        boolean delay;
        boolean failUnbind;
        boolean refuseBind;
        boolean disconnectDuringBind;
        volatile ServiceInfo resolved = publicService();
        final IBinder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) {
                if (code != 42) return false;
                reply.writeNoException();
                reply.writeInt(request.readInt() + 1);
                return true;
            }
        };

        ServiceContext() { super(null); }

        private static ServiceInfo publicService() {
            ServiceInfo info = new ServiceInfo();
            info.packageName = PACKAGE;
            info.name = "fixture.Service";
            info.exported = true;
            info.enabled = true;
            return info;
        }

        @Override public boolean bindService(Intent intent, ServiceConnection connection, int flags) {
            check(PACKAGE.equals(intent.getPackage()) && intent.getComponent() != null &&
                  "fixture.Service".equals(intent.getComponent().getClassName()) && flags == Context.BIND_AUTO_CREATE,
                  "server receives explicit GMS intent");
            binds++;
            entered.countDown();
            if (refuseBind) return false;
            if (disconnectDuringBind) {
                connection.onServiceDisconnected(null);
                return true;
            }
            if (delay) {
                try { await(release, "release delayed server bind"); }
                catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException(e);
                }
            }
            connection.onServiceConnected(new ComponentName(PACKAGE, "fixture.Service"), service);
            return true;
        }

        @Override public void unbindService(ServiceConnection connection) {
            unbinds++;
            if (failUnbind) throw new IllegalStateException("fixture unbind failure");
        }
    }

    private static final class Transport implements AutoCloseable {
        final ServiceContext context = new ServiceContext();
        final SharedMicrogAccounts.Server accountServer = new SharedMicrogAccounts.Server(
            new SharedMicrogAccounts.Engine() {
                @Override public android.accounts.Account[] accounts() {
                    return new android.accounts.Account[] {
                        new android.accounts.Account("fixture.google", SharedMicrogAccounts.GOOGLE)
                    };
                }
                @Override public AutoCloseable start(int operation, android.accounts.Account account,
                        String tokenType, String[] features, android.os.Bundle options,
                        android.accounts.AccountAuthenticatorResponse response) {
                    return () -> { };
                }
            }, new SharedMicrogAccounts.Foreground() {
                @Override public boolean launch(long channelId, android.content.Intent intent) { return false; }
                @Override public void dismiss(long channelId) { }
            }, 1, "fixture.client", 11001, 50001);
        final SharedMicrogServiceRouter serverRouter =
            new SharedMicrogServiceRouter(context, intent -> context.resolved, accountServer);
        final Socket client;
        final Socket server;
        final BinderBridge serverBridge;
        final AtomicBoolean clientShutdown = new AtomicBoolean();
        SharedMicrogClientSession session;

        Transport() throws Exception {
            InetAddress loopback = InetAddress.getLoopbackAddress();
            try (ServerSocket listener = new ServerSocket(0, 1, loopback)) {
                client = new Socket(loopback, listener.getLocalPort());
                server = listener.accept();
            }
            serverBridge = new BinderBridge(server.getInputStream(), server.getOutputStream(),
                serverRouter.root(), shutdown(server));
        }

        Closeable shutdown(Socket socket) {
            return () -> {
                if (socket == client) clientShutdown.set(true);
                if (!socket.isClosed()) {
                    if (!socket.isInputShutdown()) socket.shutdownInput();
                    if (!socket.isOutputShutdown()) socket.shutdownOutput();
                }
            };
        }

        void open() throws Exception {
            session = SharedMicrogClientSession.open(client.getInputStream(), client.getOutputStream(),
                shutdown(client), Runnable::run);
        }

        @Override public void close() throws Exception {
            context.release.countDown();
            try { if (session != null) session.close(); }
            finally {
                serverBridge.close();
                serverBridge.awaitClosed();
                serverRouter.close();
                client.close();
                server.close();
            }
        }
    }

    private static ServiceConnection connection(AtomicReference<IBinder> service, CountDownLatch disconnected) {
        return new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) { service.set(binder); }
            @Override public void onServiceDisconnected(ComponentName name) { disconnected.countDown(); }
        };
    }

    private static Intent intent() { return new Intent(ACTION).setPackage(PACKAGE); }

    private static void bindAndTeardown() throws Exception {
        try (Transport transport = new Transport()) {
            transport.open();
            java.lang.reflect.Field clientUid = SharedMicrogIdentity.class.getDeclaredField("clientUid");
            clientUid.setAccessible(true);
            clientUid.setInt(null, 11001);
            android.accounts.Account[] accounts = SharedMicrogAccounts.currentClient().accounts();
            check(accounts.length == 1 && "fixture.google".equals(accounts[0].name),
                "account API is exposed through the manager-authorized root channel");
            android.accounts.AccountManager manager = new android.accounts.AccountManager(null);
            android.accounts.Account[] managerAccounts = manager.getAccountsByType(SharedMicrogAccounts.GOOGLE);
            check(managerAccounts.length == 1 && accounts[0].equals(managerAccounts[0]),
                "AccountManager lists worker-owned Google accounts");
            boolean secretRejected = false;
            try { manager.getPassword(managerAccounts[0]); }
            catch (UnsupportedOperationException expected) { secretRejected = true; }
            check(secretRejected, "AccountManager does not read worker credentials locally");
            check(!manager.hasAccountAccess(managerAccounts[0], "fixture.client", null) &&
                    manager.getAccountVisibility(managerAccounts[0], "fixture.client") ==
                        android.accounts.AccountManager.VISIBILITY_UNDEFINED,
                "account presence does not imply per-client visibility or access");
            boolean unsupported = false;
            try {
                Services.bind(null, intent().addCategory("unsupported.category"),
                    connection(new AtomicReference<>(), new CountDownLatch(1)));
            } catch (IllegalArgumentException expected) { unsupported = true; }
            check(unsupported && transport.context.binds == 0,
                  "shared mode cannot silently route unsupported GMS intent locally");
            AtomicReference<IBinder> service = new AtomicReference<>();
            ServiceConnection connection = connection(service, new CountDownLatch(1));
            check(Services.bind(null, intent(), connection), "Services routes bind through installed session");
            check(service.get() != null && transport.context.binds == 1, "real server router returns service binder");
            Parcel request = Parcel.obtain(), reply = Parcel.obtain();
            try {
                request.writeInt(9);
                check(service.get().transact(42, request, reply, 0), "imported service binder transaction");
                reply.readException();
                check(reply.readInt() == 10, "service binder reply crosses bridge");
            } finally { request.recycle(); reply.recycle(); }
            Services.unbind(connection);
            check(transport.context.unbinds == 1, "Services routes UNBIND directly to server root");
            check(Services.bind(null, intent(), connection), "second bind");
            transport.session.close();
            check(transport.context.unbinds == 2 && transport.clientShutdown.get(),
                  "router unbind precedes transport shutdown");
            transport.session.close();
            check(transport.context.unbinds == 2, "close is idempotent");
            clientUid.setInt(null, -1);
        }
    }

    private static void startupFailures(Field mainLooper) throws Exception {
        Object main = mainLooper.get(null);
        try (Transport transport = new Transport()) {
            mainLooper.set(null, null);
            boolean failed = false;
            try {
                SharedMicrogClientSession.open(transport.client.getInputStream(),
                    transport.client.getOutputStream(), transport.shutdown(transport.client));
            } catch (IllegalStateException expected) { failed = true; }
            finally { mainLooper.set(null, main); }
            check(failed && transport.clientShutdown.get(), "missing main looper closes owned transport");
        }
        try (Transport first = new Transport(); Transport second = new Transport()) {
            first.open();
            boolean failed = false;
            try { second.open(); }
            catch (IllegalStateException expected) { failed = true; }
            check(failed && second.clientShutdown.get(), "duplicate installation closes new transport");
            check(Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))),
                  "failed installation preserves original router");
        }
    }

    private static void refusedAndDisconnectedBindings() throws Exception {
        try (Transport transport = new Transport()) {
            transport.open();
            AtomicReference<IBinder> service = new AtomicReference<>();
            ServiceConnection connection = connection(service, new CountDownLatch(1));
            transport.context.refuseBind = true;
            check(!Services.bind(null, intent(), connection), "refused service returns false across the bridge");
            check(service.get() == null && transport.context.unbinds == 0,
                  "refused service creates no callback or local service binding");
            Field bindings = SharedMicrogServiceRouter.class.getDeclaredField("bindings");
            bindings.setAccessible(true);
            check(((java.util.Map<?, ?>) bindings.get(transport.serverRouter)).isEmpty(),
                  "client releases refused server binding");
            transport.context.refuseBind = false;
            transport.context.disconnectDuringBind = true;
            check(!Services.bind(null, intent(), connection), "disconnect during bind returns false");
            check(transport.context.unbinds == 1, "disconnected server binding is released");
            check(((java.util.Map<?, ?>) bindings.get(transport.serverRouter)).isEmpty(),
                  "client releases disconnected server binding");
            transport.context.disconnectDuringBind = false;
            check(Services.bind(null, intent(), connection) && service.get() != null,
                  "connection can be reused after refused binds");
            Services.unbind(connection);
            check(transport.context.unbinds == 2, "subsequent successful bind is released");
        }
    }

    private static void rejectNonPublicServices() throws Exception {
        try (Transport transport = new Transport()) {
            transport.open();
            AtomicReference<IBinder> service = new AtomicReference<>();
            ServiceConnection connection = connection(service, new CountDownLatch(1));
            ServiceInfo info = ServiceContext.publicService();
            transport.context.resolved = null;
            check(!Services.bind(null, intent(), connection), "unknown service rejected");
            transport.context.resolved = info;
            info.exported = false;
            check(!Services.bind(null, intent(), connection), "non-exported service rejected");
            info.exported = true;
            info.enabled = false;
            check(!Services.bind(null, intent(), connection), "disabled service rejected");
            info.enabled = true;
            info.permission = "fixture.permission";
            check(!Services.bind(null, intent(), connection), "permission-protected service rejected");
            info.permission = null;
            info.packageName = "fixture.other";
            check(!Services.bind(null, intent(), connection), "foreign service rejected");
            info.packageName = PACKAGE;
            check(!Services.bind(null, intent().setClassName(PACKAGE, "fixture.Other"), connection),
                  "mismatched explicit service rejected");
            check(transport.context.binds == 0 && service.get() == null,
                  "rejected requests never reach service");
            check(Services.bind(null, intent(), connection) && service.get() != null,
                  "public service remains bindable");
            Services.unbind(connection);
        }
    }

    private static void replacementAndDisconnect() throws Exception {
        try (Transport first = new Transport(); Transport second = new Transport()) {
            first.open();
            BinderBridge replacementBridge = new BinderBridge(second.client.getInputStream(),
                second.client.getOutputStream(), null, second.shutdown(second.client));
            SharedMicrogClientRouter replacement = new SharedMicrogClientRouter(replacementBridge.remoteRoot(),
                Runnable::run);
            try {
                Services.setSharedMicrogClientRouter(replacement);
                first.session.close();
                check(Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))) &&
                      second.context.binds == 1, "old session compare-and-clear preserves replacement router");
            } finally {
                Services.clearSharedMicrogClientRouter(replacement);
                replacement.close();
                replacementBridge.close();
                replacementBridge.awaitClosed();
            }
        }
        try (Transport transport = new Transport()) {
            transport.open();
            CountDownLatch disconnected = new CountDownLatch(1);
            check(Services.bind(null, intent(), connection(new AtomicReference<>(), disconnected)), "disconnect bind");
            transport.serverBridge.close();
            await(disconnected, "unexpected disconnect callback delivered");
            boolean failedClosed = false;
            try { Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))); }
            catch (IllegalStateException expected) { failedClosed = true; }
            check(failedClosed, "unexpected disconnect cannot fall back to local MicroG");
        }
    }

    private static void pendingBindAndClose() throws Exception {
        try (Transport transport = new Transport()) {
            transport.context.delay = true;
            transport.open();
            AtomicReference<IBinder> service = new AtomicReference<>();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread binder = new Thread(() -> {
                try { Services.bind(null, intent(), connection(service, new CountDownLatch(1))); }
                catch (Throwable e) { failure.set(e); }
            });
            binder.start();
            await(transport.context.entered, "pending bind reaches server");
            System.out.println("Expected fixture cancellation: pending server bind may be interrupted.");
            transport.session.close();
            transport.context.release.countDown();
            binder.join(5000);
            check(!binder.isAlive() && failure.get() instanceof IllegalStateException && service.get() == null,
                  "close interrupts pending bind without delivering stale service");
        }
        try (Transport transport = new Transport()) {
            transport.open();
            check(Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))),
                  "unbind error fixture");
            transport.context.failUnbind = true;
            System.out.println("Expected fixture error: server UNBIND throws; client must still close transport.");
            boolean failed = false;
            try { transport.session.close(); }
            catch (IllegalStateException expected) { failed = true; }
            check(failed && transport.clientShutdown.get(), "teardown error still shuts down transport");
        }
    }

    private static void interruptedClose() throws Exception {
        try (Transport transport = new Transport()) {
            transport.open();
            Thread.currentThread().interrupt();
            transport.session.close();
            check(Thread.interrupted() && transport.clientShutdown.get(),
                  "close completes transport shutdown and restores interrupt");
        }
    }

    private static void requiredModeDoesNotFallback() throws Exception {
        Field required = SharedMicrogClientStartup.class.getDeclaredField("required");
        required.setAccessible(true);
        required.setBoolean(null, true);
        try (Transport transport = new Transport()) {
            boolean missing = false;
            try { Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))); }
            catch (IllegalStateException expected) { missing = true; }
            check(missing, "required shared mode rejects bind before router installation");
            transport.open();
            transport.session.close();
            boolean closed = false;
            try { Services.bind(null, intent(), connection(new AtomicReference<>(), new CountDownLatch(1))); }
            catch (IllegalStateException expected) { closed = true; }
            check(closed, "required shared mode rejects bind after explicit teardown");
        } finally {
            required.setBoolean(null, false);
        }
    }

    public static void main(String[] args) throws Exception {
        Field self = ApplicationInfo.class.getDeclaredField("sSelf");
        self.setAccessible(true);
        Object oldSelf = self.get(null);
        self.set(null, new ApplicationInfo());
        Field mainLooper = Looper.class.getDeclaredField("sMain");
        mainLooper.setAccessible(true);
        Object oldMain = mainLooper.get(null);
        Constructor<Looper> constructor = Looper.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        // Host JVM has no guest natives; only provide the main-loop identity for the server guard.
        mainLooper.set(null, constructor.newInstance());
        try {
            bindAndTeardown();
            refusedAndDisconnectedBindings();
            rejectNonPublicServices();
            startupFailures(mainLooper);
            replacementAndDisconnect();
            pendingBindAndClose();
            interruptedClose();
            requiredModeDoesNotFallback();
            System.out.println("SharedMicrogClientSessionTest: PASS Services, bridge, root router, startup errors, disconnect and close races");
        } finally {
            Services.setSharedMicrogClientRouter(null);
            mainLooper.set(null, oldMain);
            self.set(null, oldSelf);
        }
    }
}
