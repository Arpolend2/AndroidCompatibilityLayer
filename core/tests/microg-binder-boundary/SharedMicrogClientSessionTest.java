package megler;

import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.ContentProvider;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import java.io.Closeable;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
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
        int starts;
        int stops;
        Intent boundIntent;
        Intent startedIntent;
        Intent stoppedIntent;
        boolean stopResult = true;
        volatile ServiceInfo resolved = publicService();
        volatile ProviderInfo resolvedProvider;
        volatile ContentProvider contentProvider;
        final IBinder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags) {
                if (code != 42) return false;
                reply.writeNoException();
                reply.writeInt(request.readInt() + 1);
                return true;
            }
        };

        ServiceContext() { super(null); }

        @Override public android.content.ContentResolver getContentResolver() {
            return android.content.ContentResolver.self();
        }

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
            boundIntent = intent;
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

        @Override public ComponentName startService(Intent intent) {
            starts++;
            startedIntent = intent;
            return intent.getComponent();
        }

        @Override public boolean stopService(Intent intent) {
            stops++;
            stoppedIntent = intent;
            return stopResult;
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
                @Override public long launch(long channelId, android.content.Intent intent) { return 0; }
                @Override public void dismiss(long channelId, long generation) { }
            }, 1, "fixture.client", 11001, 50001);
        final AtomicReference<SharedMicrogServiceRouter.OrderedCompletion> orderedCompletion = new AtomicReference<>();
        final AtomicReference<Intent> orderedIntent = new AtomicReference<>();
        final AtomicReference<Bundle> orderedExtras = new AtomicReference<>();
        final AtomicReference<Integer> orderedCode = new AtomicReference<>();
        final AtomicReference<String> orderedData = new AtomicReference<>();
        final SharedMicrogServiceRouter serverRouter = new SharedMicrogServiceRouter(context,
            intent -> context.resolved, accountServer, new SharedMicrogServiceRouter.ProviderResolver() {
                @Override public ProviderInfo resolve(String authority) {
                    ProviderInfo info = context.resolvedProvider;
                    return info != null && authority.equals(info.authority) ? info : null;
                }
                @Override public ContentProvider provider(String authority) {
                    ProviderInfo info = context.resolvedProvider;
                    return info != null && authority.equals(info.authority) ? context.contentProvider : null;
                }
            }, (intent, code, data, extras, completion) -> {
                orderedIntent.set(intent);
                orderedCode.set(code);
                orderedData.set(data);
                orderedExtras.set(extras);
                orderedCompletion.set(completion);
            });
        final Socket client;
        final Socket server;
        final BinderBridge serverBridge;
        final AtomicBoolean clientShutdown = new AtomicBoolean();
        final AtomicReference<Intent> workerBroadcast = new AtomicReference<>();
        final AtomicReference<String> workerPermission = new AtomicReference<>();
        final AtomicReference<String> workerAlso = new AtomicReference<>();
        final AtomicReference<Boolean> workerHoldsAlso = new AtomicReference<>();
        final AtomicReference<Integer> workerUid = new AtomicReference<>();
        final AtomicReference<String> workerPackage = new AtomicReference<>();
        final CountDownLatch workerBroadcastReceived = new CountDownLatch(1);
        SharedMicrogClientSession session;

        Transport() throws Exception {
            InetAddress loopback = InetAddress.getLoopbackAddress();
            try (ServerSocket listener = new ServerSocket(0, 1, loopback)) {
                client = new Socket(loopback, listener.getLocalPort());
                server = listener.accept();
            }
            serverBridge = new BinderBridge(server.getInputStream(), server.getOutputStream(),
                serverRouter.root(), shutdown(server), 11001, 50001);
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
                shutdown(client), Runnable::run, (intent, permission, also, holdsAlso, uid, packageName) -> {
                    workerBroadcast.set(intent);
                    workerPermission.set(permission);
                    workerAlso.set(also);
                    workerHoldsAlso.set(holdsAlso);
                    workerUid.set(uid);
                    workerPackage.set(packageName);
                    workerBroadcastReceived.countDown();
                });
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
                Intent unsupportedIntent = intent();
                unsupportedIntent.setSelector(new Intent("unsupported.selector"));
                Services.bind(null, unsupportedIntent,
                    connection(new AtomicReference<>(), new CountDownLatch(1)));
            } catch (IllegalArgumentException expected) { unsupported = true; }
            check(unsupported && transport.context.binds == 0,
                  "shared mode cannot silently route unsupported GMS intent locally");
            AtomicReference<IBinder> service = new AtomicReference<>();
            ServiceConnection connection = connection(service, new CountDownLatch(1));
            Intent bindIntent = intent().setDataAndType(Uri.parse("content://example/bind"),
                "application/octet-stream").addCategory("fixture.category").putExtra("request", "bind-data");
            check(Services.bind(null, bindIntent, connection), "Services routes bind through installed session");
            check(service.get() != null && transport.context.binds == 1, "real server router returns service binder");
            check(Uri.parse("content://example/bind").equals(transport.context.boundIntent.getData()) &&
                  "application/octet-stream".equals(transport.context.boundIntent.getType()) &&
                  transport.context.boundIntent.getCategories().contains("fixture.category") &&
                  "bind-data".equals(transport.context.boundIntent.getStringExtra("request")),
                  "full bind Intent survives the Binder bridge");
            Parcel request = Parcel.obtain(), reply = Parcel.obtain();
            try {
                request.writeInt(9);
                check(service.get().transact(42, request, reply, 0), "imported service binder transaction");
                reply.readException();
                check(reply.readInt() == 10, "service binder reply crosses bridge");
            } finally { request.recycle(); reply.recycle(); }
            Services.unbind(connection);
            check(transport.context.unbinds == 1, "Services routes UNBIND directly to server root");
            Intent serviceIntent = intent().setDataAndType(Uri.parse("content://example/start"),
                "application/octet-stream").addCategory("fixture.category").putExtra("request", "start-data");
            ComponentName started = Services.start(null, serviceIntent);
            check(transport.context.starts == 1 && started != null &&
                  Uri.parse("content://example/start").equals(transport.context.startedIntent.getData()) &&
                  "application/octet-stream".equals(transport.context.startedIntent.getType()) &&
                  transport.context.startedIntent.getCategories().contains("fixture.category") &&
                  "start-data".equals(transport.context.startedIntent.getStringExtra("request")),
                  "Services routes startService with its full Intent");
            check(Services.stop(serviceIntent) && transport.context.stops == 1 &&
                  "start-data".equals(transport.context.stoppedIntent.getStringExtra("request")),
                  "Services routes stopService and preserves its result");
            Intent workerIntent = new Intent("fixture.worker.BROADCAST").setPackage("fixture.client")
                .putExtra("marker", "worker-to-client");
            transport.serverRouter.broadcastToClient(workerIntent, "fixture.permission", "fixture.also",
                true, 11001, PACKAGE);
            await(transport.workerBroadcastReceived, "worker broadcast callback crosses the Binder bridge");
            check("fixture.worker.BROADCAST".equals(transport.workerBroadcast.get().getAction()) &&
                    "fixture.client".equals(transport.workerBroadcast.get().getPackage()) &&
                    "worker-to-client".equals(transport.workerBroadcast.get().getStringExtra("marker")) &&
                    "fixture.permission".equals(transport.workerPermission.get()) &&
                    "fixture.also".equals(transport.workerAlso.get()) &&
                    Boolean.TRUE.equals(transport.workerHoldsAlso.get()) &&
                    Integer.valueOf(11001).equals(transport.workerUid.get()) &&
                    PACKAGE.equals(transport.workerPackage.get()),
                  "worker broadcast preserves targeting, filters and sender identity");
            boolean unsupportedExtraRejected = false;
            try {
                Services.start(null, intent().putExtra("unsupported", new java.util.Date()));
            } catch (IllegalStateException expected) { unsupportedExtraRejected = true; }
            check(unsupportedExtraRejected && transport.context.starts == 1,
                  "unsupported Bundle extras fail explicitly before reaching the worker");
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
            CountDownLatch closed = new CountDownLatch(1);
            AtomicReference<Throwable> closeFailure = new AtomicReference<>();
            Thread closer = new Thread(() -> {
                try { transport.session.close(); }
                catch (Throwable e) { closeFailure.set(e); }
                finally { closed.countDown(); }
            });
            closer.start();
            boolean closedPromptly;
            try { closedPromptly = closed.await(2, TimeUnit.SECONDS); }
            finally { transport.context.release.countDown(); }
            closer.join(5000);
            binder.join(5000);
            check(closedPromptly && closeFailure.get() == null && !closer.isAlive() &&
                    !binder.isAlive() && failure.get() instanceof IllegalStateException && service.get() == null,
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

    private static void providerRoundTrip() throws Exception {
        final String authority = "com.google.fixture.provider";
        Field required = SharedMicrogClientStartup.class.getDeclaredField("required");
        required.setAccessible(true);
        required.setBoolean(null, true);
        Field worker = SharedMicrogIdentity.class.getDeclaredField("worker");
        worker.setAccessible(true);
        worker.setBoolean(null, false);
        SharedMicrogIdentity.startWorker();
        SharedMicrogIdentity.add(1, "fixture.client", 11001);
        AtomicReference<Long> batchBackReference = new AtomicReference<>();
        java.nio.file.Path providerFile = java.nio.file.Files.createTempFile("shared-microg-provider-", ".test");
        try (Transport transport = new Transport()) {
            ProviderInfo info = new ProviderInfo();
            info.packageName = PACKAGE;
            info.name = "fixture.Provider";
            info.authority = authority;
            info.exported = true;
            info.enabled = true;
            transport.context.resolvedProvider = info;
            transport.context.contentProvider = new ContentProvider() {
                @Override public boolean onCreate() { return true; }
                @Override public android.database.Cursor query(Uri uri, String[] projection, String selection,
                        String[] args, String sort) {
                    android.database.MatrixCursor cursor = new android.database.MatrixCursor(
                        projection == null ? new String[] { "_id", "caller" } : projection);
                    cursor.addRow(new Object[] { 7L, getCallingPackage() });
                    return cursor;
                }
                @Override public String getType(Uri uri) { return "vnd.fixture.provider"; }
                @Override public Uri insert(Uri uri, android.content.ContentValues values) {
                    return Uri.parse(uri.toString() + "/" + values.getAsInteger("count"));
                }
                @Override public int delete(Uri uri, String selection, String[] args) { return args == null ? 0 : args.length; }
                @Override public int update(Uri uri, android.content.ContentValues values, String selection,
                        String[] args) {
                    if (values.containsKey("from_insert"))
                        batchBackReference.set(values.getAsLong("from_insert"));
                    return values.size();
                }
                @Override public android.os.ParcelFileDescriptor openFile(Uri uri, String mode)
                        throws java.io.FileNotFoundException {
                    return android.os.ParcelFileDescriptor.open(providerFile.toFile(),
                        android.os.ParcelFileDescriptor.parseMode(mode));
                }
                @Override public Bundle call(String method, String arg, Bundle extras) {
                    Bundle result = new Bundle();
                    result.putString("caller", getCallingPackage());
                    result.putString("method", method);
                    result.putString("arg", arg);
                    result.putString("input", extras == null ? null : extras.getString("input"));
                    return result;
                }
            };
            transport.context.contentProvider.attachInfo(null, info);
            transport.open();
            boolean orderedRejected = false;
            try { Broadcasts.sendOrdered(null, intent().setData(Uri.parse("content://fixture/data")), null, null, null, 0, null, null); }
            catch (UnsupportedOperationException expected) { orderedRejected = true; }
            check(orderedRejected, "shared ordered broadcasts are not sent as ordinary broadcasts");
            orderedRejected = false;
            try { transport.context.sendOrderedBroadcastMultiplePermissions(
                intent(), new String[0], null, null, null, 0, null, null, null); }
            catch (UnsupportedOperationException expected) { orderedRejected = true; }
            check(orderedRejected, "ContextWrapper rejects unsupported shared ordered overloads");
            Uri uri = Uri.parse("content://" + authority + "/items");
            android.database.Cursor cursor = android.content.ContentResolver.self().query(uri,
                new String[] { "_id", "caller" }, null, null, null);
            check(cursor != null && cursor.getCount() == 1 && cursor.moveToFirst() &&
                    cursor.getLong(0) == 7 && "fixture.client".equals(cursor.getString(1)),
                  "provider query crosses the channel with the authenticated client package");
            cursor.close();
            check("vnd.fixture.provider".equals(android.content.ContentResolver.self().getType(uri)),
                  "provider MIME type crosses the channel");
            Bundle input = new Bundle();
            input.putString("input", "value");
            Bundle result = android.content.ContentResolver.self().call(authority, "fixture.method", "argument", input);
            check(result != null && "fixture.client".equals(result.getString("caller")) &&
                    "fixture.method".equals(result.getString("method")) &&
                    "argument".equals(result.getString("arg")) &&
                    "value".equals(result.getString("input")),
                  "provider call preserves supported Bundle data and caller identity");
            android.content.ContentValues values = new android.content.ContentValues();
            values.put("count", 3);
            values.put("name", "fixture");
            Uri inserted = android.content.ContentResolver.self().insert(uri, values);
            check(inserted != null && inserted.toString().endsWith("/3"),
                  "provider insert preserves supported ContentValues");
            check(android.content.ContentResolver.self().update(uri, values, null, new String[] { "a" }) == 2 &&
                    android.content.ContentResolver.self().delete(uri, null, new String[] { "a", "b" }) == 2,
                  "provider update and delete results cross the channel");
            ArrayList<android.content.ContentProviderOperation> operations = new ArrayList<>();
            operations.add(android.content.ContentProviderOperation.newInsert(uri)
                .withValue("count", 7).build());
            operations.add(android.content.ContentProviderOperation.newUpdate(uri)
                .withValueBackReference("from_insert", 0)
                .withSelection("count = ?", new String[] { "7" })
                .withExpectedCount(1).build());
            operations.add(android.content.ContentProviderOperation.newCall(uri, "batch.call", "arg")
                .withExtra("input", "batch").build());
            android.content.ContentProviderResult[] batch =
                android.content.ContentResolver.self().applyBatch(authority, operations);
            check(batch.length == 3 && batch[0].uri.toString().endsWith("/7") &&
                    Integer.valueOf(1).equals(batch[1].count) && batch[2].extras != null &&
                    "batch".equals(batch[2].extras.getString("input")) &&
                    Long.valueOf(7).equals(batchBackReference.get()),
                  "provider batches preserve operations, back references and typed results");
            Bundle combinedExtras = new Bundle();
            combinedExtras.putString("key", "value");
            Parcel combinedParcel = Parcel.obtain();
            combinedParcel.writeTypedObject(new android.content.ContentProviderResult(
                uri, 3, combinedExtras, new IllegalStateException("fixture")), 0);
            byte[] combinedWire = combinedParcel.meglerMarshallBinders(binder -> 1);
            Parcel combinedRead = Parcel.meglerUnmarshallBinders(combinedWire, handle -> null);
            android.content.ContentProviderResult combined =
                combinedRead.readTypedObject(android.content.ContentProviderResult.CREATOR);
            check(uri.toString().equals(combined.uri.toString()) && combined.count == 3 &&
                    "value".equals(combined.extras.getString("key")) && combined.exception != null,
                  "provider result preserves all populated fields across Binder");
            combinedParcel.recycle();
            combinedRead.recycle();
            ArrayList<android.content.ContentProviderOperation> failure = new ArrayList<>();
            failure.add(android.content.ContentProviderOperation.newAssertQuery(uri)
                .withExpectedCount(1).build());
            failure.add(android.content.ContentProviderOperation.newDelete(uri)
                .withSelection(null, new String[] { "a" }).withExpectedCount(1).build());
            android.content.ContentProviderResult[] assertion =
                android.content.ContentResolver.self().applyBatch(authority, failure);
            check(assertion.length == 2 && Integer.valueOf(1).equals(assertion[0].count) &&
                    Integer.valueOf(1).equals(assertion[1].count),
                  "provider assertion and delete cross the batch channel");
            failure.clear();
            failure.add(android.content.ContentProviderOperation.newDelete(uri)
                .withExpectedCount(1).build());
            boolean batchFailed = false;
            try { android.content.ContentResolver.self().applyBatch(authority, failure); }
            catch (android.content.OperationApplicationException expected) { batchFailed = true; }
            check(batchFailed, "provider batch exceptions cross the channel");
            failure.clear();
            failure.add(android.content.ContentProviderOperation.newDelete(uri)
                .withExpectedCount(1).withExceptionAllowed(true).build());
            check(android.content.ContentResolver.self().applyBatch(authority, failure)[0].exception != null,
                  "allowed provider batch exceptions return as results");
            failure.clear();
            failure.add(android.content.ContentProviderOperation.newDelete(
                Uri.parse("content://com.google.other/items")).build());
            boolean authorityRejected = false;
            try { android.content.ContentResolver.self().applyBatch(authority, failure); }
            catch (android.content.OperationApplicationException expected) { authorityRejected = true; }
            check(authorityRejected, "provider batch rejects operations for another authority");
            AtomicReference<Uri> notification = new AtomicReference<>();
            CountDownLatch observed = new CountDownLatch(1);
            CountDownLatch clientObserved = new CountDownLatch(1);
            java.util.concurrent.atomic.AtomicInteger notificationCount = new java.util.concurrent.atomic.AtomicInteger();
            android.database.ContentObserver observer = new android.database.ContentObserver(null) {
                @Override public void onChange(boolean self, Uri changed) {
                    notification.set(changed);
                    if (notificationCount.incrementAndGet() == 1) observed.countDown();
                    else clientObserved.countDown();
                }
            };
            android.content.ContentResolver resolver = android.content.ContentResolver.self();
            resolver.registerContentObserver(uri, true, observer);
            Uri unrelated = Uri.parse("content://" + authority + "/items-other");
            resolver.meglerNotifyLocalChange(unrelated, null);
            check(!observed.await(200, TimeUnit.MILLISECONDS), "observer rejects path prefix collisions");
            Uri child = Uri.parse(uri.toString() + "/child");
            resolver.meglerNotifyLocalChange(child, null);
            await(observed, "worker provider notification reaches client");
            check(child.toString().equals(notification.get().toString()), "observer preserves changed URI");
            resolver.notifyChange(child, observer);
            check(!clientObserved.await(200, TimeUnit.MILLISECONDS), "origin observer is excluded");
            resolver.notifyChange(child, null);
            await(clientObserved, "client notification is relayed to worker observers");
            boolean flagsRejected = false;
            try { resolver.notifyChange(child, null, android.content.ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS); }
            catch (UnsupportedOperationException expected) { flagsRejected = true; }
            check(flagsRejected, "unsupported shared provider notification flags are rejected");
            resolver.unregisterContentObserver(observer);
            notification.set(null);
            resolver.meglerNotifyLocalChange(child, null);
            Thread.sleep(200);
            check(notification.get() == null, "unregistered provider observer receives no further events");
            byte[] written = new byte[70000];
            for (int i = 0; i < written.length; i++) written[i] = (byte) i;
            try (java.io.OutputStream output = resolver.openOutputStream(uri, "wt")) {
                output.write(written);
            }
            check(java.util.Arrays.equals(written, java.nio.file.Files.readAllBytes(providerFile)),
                  "provider output stream writes multiple bounded chunks");
            try (java.io.OutputStream output = resolver.openOutputStream(uri, "wa")) {
                output.write(123);
            }
            byte[] appended = java.nio.file.Files.readAllBytes(providerFile);
            check(appended.length == written.length + 1 && appended[written.length] == 123,
                  "provider output stream preserves append mode");
            boolean writeRejected = false;
            try { android.content.ContentResolver.self().openFileDescriptor(uri, "w"); }
            catch (UnsupportedOperationException expected) { writeRejected = true; }
            check(writeRejected, "provider file writes are explicitly rejected");
            info.readPermission = "fixture.permission";
            boolean permissionRejected = false;
            try { android.content.ContentResolver.self().query(uri, null, null, null, null); }
            catch (IllegalStateException expected) { permissionRejected = true; }
            check(permissionRejected, "permission-protected providers fail closed");
            info.readPermission = null;
            resolver.registerContentObserver(uri, true, observer);
            java.io.OutputStream abandoned = resolver.openOutputStream(uri, "w");
            transport.serverRouter.close();
            notification.set(null);
            resolver.meglerNotifyLocalChange(child, null);
            Thread.sleep(200);
            check(notification.get() == null, "router shutdown removes worker observer registrations");
            boolean closedStreamFailed = false;
            try { abandoned.write(1); }
            catch (java.io.IOException expected) { closedStreamFailed = true; }
            check(closedStreamFailed, "router shutdown closes unfinished provider writes");
        } finally {
            java.nio.file.Files.deleteIfExists(providerFile);
            SharedMicrogIdentity.remove(1);
            worker.setBoolean(null, false);
            required.setBoolean(null, false);
        }
    }

    private static void providerCallingPackageIdentity() throws Exception {
        android.content.ContentProvider provider = new android.content.ContentProvider() {
            @Override public boolean onCreate() { return true; }
            @Override public android.database.Cursor query(Uri uri, String[] projection, String selection,
                    String[] args, String sort) { return null; }
            @Override public String getType(Uri uri) { return null; }
            @Override public Uri insert(Uri uri, android.content.ContentValues values) { return null; }
            @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
            @Override public int update(Uri uri, android.content.ContentValues values, String selection,
                    String[] args) { return 0; }
        };
        Field worker = SharedMicrogIdentity.class.getDeclaredField("worker");
        worker.setAccessible(true);
        worker.setBoolean(null, false);
        Field clientUid = SharedMicrogIdentity.class.getDeclaredField("clientUid");
        clientUid.setAccessible(true);
        clientUid.setInt(null, -1);
        java.lang.reflect.Field channels = SharedMicrogIdentity.class.getDeclaredField("channels");
        channels.setAccessible(true);
        ((java.util.Map<?, ?>) channels.get(null)).clear();
        java.lang.reflect.Field byUid = SharedMicrogIdentity.class.getDeclaredField("byUid");
        byUid.setAccessible(true);
        ((java.util.Map<?, ?>) byUid.get(null)).clear();
        java.lang.reflect.Field byPackage = SharedMicrogIdentity.class.getDeclaredField("byPackage");
        byPackage.setAccessible(true);
        ((java.util.Map<?, ?>) byPackage.get(null)).clear();
        SharedMicrogIdentity.startWorker();
        SharedMicrogIdentity.add(1, "fixture.client", 11001);
        SharedMicrogIdentity.add(2, "fixture.other", 11002);
        final String[] observed = new String[3];
        IBinder nested = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws android.os.RemoteException {
                observed[1] = provider.getCallingPackage();
                throw new android.os.RemoteException("expected nested failure");
            }
        };
        IBinder outer = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws android.os.RemoteException {
                observed[0] = provider.getCallingPackage();
                Parcel request = Parcel.obtain(), nestedReply = Parcel.obtain();
                try {
                    Binder.meglerTransactWithCaller(nested, 1, request, nestedReply, 0, 11002, 50002);
                } catch (android.os.RemoteException expected) {
                    observed[2] = provider.getCallingPackage();
                } finally {
                    request.recycle();
                    nestedReply.recycle();
                }
                throw new android.os.RemoteException("expected outer failure");
            }
        };
        Field callerIdentity = Binder.class.getDeclaredField("sCaller");
        callerIdentity.setAccessible(true);
        ThreadLocal<?> callerState = (ThreadLocal<?>) callerIdentity.get(null);
        try {
            boolean failed = false;
            Parcel request = Parcel.obtain(), reply = Parcel.obtain();
            try {
                Binder.meglerTransactWithCaller(outer, 1, request, reply, 0, 11001, 50001);
            } catch (android.os.RemoteException expected) {
                failed = true;
            } finally {
                request.recycle();
                reply.recycle();
            }
            check(failed && "fixture.client".equals(observed[0]) &&
                    "fixture.other".equals(observed[1]) && "fixture.client".equals(observed[2]),
                  "provider caller identity follows nested authenticated Binder calls and restores after exceptions");
            check(callerState.get() == null,
                  "provider caller identity does not leak beyond the authenticated transaction");
        } finally {
            SharedMicrogIdentity.remove(1);
            SharedMicrogIdentity.remove(2);
            worker.setBoolean(null, false);
        }
    }

    private static void broadcastPriorities() {
        Intent intent = new Intent("fixture.action");
        Manifest.Component component = new Manifest.Component();
        Manifest.Filter low = new Manifest.Filter();
        low.actions.add("fixture.action");
        low.priority = -10;
        Manifest.Filter high = new Manifest.Filter();
        high.actions.add("fixture.action");
        high.priority = 20;
        Manifest.Filter unmatched = new Manifest.Filter();
        unmatched.actions.add("fixture.other");
        unmatched.priority = 100;
        component.filters.add(low);
        component.filters.add(unmatched);
        component.filters.add(high);
        check(Broadcasts.manifestPriority(component, intent) == 20,
              "only matching manifest filters determine receiver priority");
        check(Broadcasts.manifestPriority(component,
              new Intent("fixture.action").setComponent(new ComponentName(PACKAGE, "Receiver"))) == 0,
              "explicit broadcasts do not use filter priority");
        android.content.IntentFilter registered = new android.content.IntentFilter("fixture.action");
        registered.setPriority(10);
        Object[] manifestHigh = { "manifest-high", null, null, null, 20 };
        Object[] dynamic = { "dynamic", registered, null, null };
        Object[] manifestLow = { "manifest-low", null, null, null, -10 };
        Object[] equal = { "manifest-equal", null, null, null, 20 };
        ArrayList<Object[]> targets = new ArrayList<>();
        targets.add(manifestLow);
        targets.add(manifestHigh);
        targets.add(dynamic);
        targets.add(equal);
        Broadcasts.sortTargets(targets);
        check(targets.get(0) == manifestHigh && targets.get(1) == equal &&
              targets.get(2) == dynamic && targets.get(3) == manifestLow,
              "manifest and dynamic receivers are ordered by priority with stable ties");
    }

    private static void orderedBroadcastRoundTrip() throws Exception {
        try (Transport transport = new Transport()) {
            transport.open();
            Intent intent = intent().setComponent(new ComponentName(PACKAGE, "fixture.Receiver"))
                .putExtra("message", "request");
            Bundle extras = new Bundle();
            extras.putString("before", "initial");
            extras.putIntArray("numbers", new int[] {1, -2});
            extras.putStringArrayList("names", new java.util.ArrayList<>(java.util.Arrays.asList("x", null)));
            CountDownLatch finished = new CountDownLatch(1);
            AtomicReference<Integer> code = new AtomicReference<>();
            AtomicReference<String> data = new AtomicReference<>();
            AtomicReference<Bundle> resultExtras = new AtomicReference<>();
            Services.sendSharedMicrogOrderedBroadcast(intent, 7, "initial", extras, (c, d, e) -> {
                code.set(c); data.set(d); resultExtras.set(e); finished.countDown();
            });
            check(transport.orderedCompletion.get() != null && finished.getCount() == 1 &&
                  "request".equals(transport.orderedIntent.get().getStringExtra("message")) &&
                  Integer.valueOf(7).equals(transport.orderedCode.get()) &&
                  "initial".equals(transport.orderedData.get()) &&
                  "initial".equals(transport.orderedExtras.get().getString("before")) &&
                  java.util.Arrays.equals(transport.orderedExtras.get().getIntArray("numbers"), new int[] {1, -2}) &&
                  transport.orderedExtras.get().getStringArrayList("names").equals(java.util.Arrays.asList("x", null)),
                  "ordered broadcast transfers the initial result and waits for completion");
            Bundle result = new Bundle();
            result.putString("after", "result");
            result.putDouble("value", 1.5);
            transport.orderedCompletion.get().finish(9, "updated", result);
            await(finished, "ordered broadcast result crosses the bridge");
            check(Integer.valueOf(9).equals(code.get()) && "updated".equals(data.get()) &&
                  "result".equals(resultExtras.get().getString("after")) && resultExtras.get().getDouble("value") == 1.5,
                  "ordered broadcast returns the changed result");
            transport.orderedCompletion.get().finish(10, "duplicate", null);
            check(Integer.valueOf(9).equals(code.get()), "ordered broadcast completion is accepted once");
            boolean rejected = false;
            try { Services.sendSharedMicrogOrderedBroadcast(intent().setData(Uri.parse("content://fixture/data")), 0, null, null, (c, d, e) -> {}); }
            catch (UnsupportedOperationException expected) { rejected = true; }
            check(rejected, "implicit shared ordered broadcasts with data fail explicitly");
            rejected = false;
            try { Broadcasts.sendOrdered(null, intent, "fixture.permission", null, null, 0, null, null); }
            catch (UnsupportedOperationException expected) { rejected = true; }
            check(rejected, "unsupported ordered permission filters fail explicitly");
            AtomicBoolean lateResult = new AtomicBoolean();
            Services.sendSharedMicrogOrderedBroadcast(intent, 0, null, null,
                (c, d, e) -> lateResult.set(true));
            SharedMicrogServiceRouter.OrderedCompletion pending = transport.orderedCompletion.get();
            transport.session.close();
            pending.finish(1, "late", null);
            check(!lateResult.get(), "closed channel drops outstanding ordered results");
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
            broadcastPriorities();
            orderedBroadcastRoundTrip();
            bindAndTeardown();
            refusedAndDisconnectedBindings();
            rejectNonPublicServices();
            startupFailures(mainLooper);
            replacementAndDisconnect();
            pendingBindAndClose();
            interruptedClose();
            requiredModeDoesNotFallback();
            providerRoundTrip();
            providerCallingPackageIdentity();
            System.out.println("SharedMicrogClientSessionTest: PASS Services, bridge, broadcasts, caller identity, startup errors, disconnect and close races");
        } finally {
            Services.setSharedMicrogClientRouter(null);
            mainLooper.set(null, oldMain);
            self.set(null, oldSelf);
        }
    }
}
