package megler;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class SharedMicrogOrderedDispatchTest {
    private static CountDownLatch received = new CountDownLatch(1);
    private static final AtomicReference<BroadcastReceiver.PendingResult> pending = new AtomicReference<>();
    private static final AtomicInteger calls = new AtomicInteger();
    private static final AtomicReference<String> order = new AtomicReference<>("");

    public static final class Receiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            if (!isOrderedBroadcast() || getResultCode() != 3 || !"initial".equals(getResultData()))
                throw new AssertionError("initial ordered result");
            calls.incrementAndGet();
            pending.set(goAsync());
            received.countDown();
        }
    }

    public static final class HighReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            order.set(order.get() + "H");
            setResultCode(getResultCode() + 1);
        }
    }

    public static final class AbortReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            order.set(order.get() + "A");
            setResultCode(getResultCode() + 1);
            abortBroadcast();
        }
    }

    public static final class SkippedReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) { order.set(order.get() + "X"); }
    }

    private static Manifest.Component implicitReceiver(Class<? extends BroadcastReceiver> type, int priority,
                                                        boolean requiresMime) {
        Manifest.Component component = new Manifest.Component();
        component.name = type.getName();
        component.kind = "receiver";
        component.enabled = true;
        component.exported = true;
        Manifest.Filter filter = new Manifest.Filter();
        filter.actions.add("fixture.implicit");
        filter.priority = priority;
        if (requiresMime) filter.mimeTypes.add("text/plain");
        component.filters.add(filter);
        return component;
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void await(CountDownLatch latch, String message) throws InterruptedException {
        check(latch.await(5, TimeUnit.SECONDS), message);
    }

    public static void main(String[] args) throws Exception {
        CountDownLatch ready = new CountDownLatch(1);
        Thread main = new Thread(() -> {
            Looper.prepareMainLooper();
            ready.countDown();
            Looper.loop();
        }, "fixture-main");
        main.start();
        await(ready, "main looper started");
        Field worker = SharedMicrogIdentity.class.getDeclaredField("worker");
        Field clientUid = SharedMicrogIdentity.class.getDeclaredField("clientUid");
        Field app = AppRunner.class.getDeclaredField("sApp");
        Field read = Manifest.class.getDeclaredField("sRead");
        worker.setAccessible(true);
        clientUid.setAccessible(true);
        app.setAccessible(true);
        read.setAccessible(true);
        Object previousApp = app.get(null);
        boolean previousRead = read.getBoolean(null);
        boolean previousWorker = worker.getBoolean(null);
        int previousUid = clientUid.getInt(null);
        try {
            worker.setBoolean(null, true);
            read.setBoolean(null, true);
            app.set(null, new Application() {
                @Override public String getPackageName() { return Native.packageName(); }
            });
            Manifest.Component component = new Manifest.Component();
            component.name = Receiver.class.getName();
            component.kind = "receiver";
            component.enabled = true;
            component.exported = true;
            Manifest.receivers.add(component);
            Intent intent = new Intent("fixture.ordered").setComponent(
                new ComponentName("com.google.android.gms", Receiver.class.getName()));
            CountDownLatch finished = new CountDownLatch(1);
            AtomicInteger code = new AtomicInteger();
            AtomicReference<String> data = new AtomicReference<>();
            AtomicReference<Bundle> extras = new AtomicReference<>();
            Broadcasts.sendOrderedFromSharedClient(intent, 3, "initial", null, (c, d, e) -> {
                code.set(c); data.set(d); extras.set(e); finished.countDown();
            });
            await(received, "exported receiver invoked");
            check(finished.getCount() == 1, "goAsync holds the final result");
            Bundle result = new Bundle();
            result.putString("result", "changed");
            pending.get().setResult(8, "done", result);
            pending.get().abortBroadcast();
            pending.get().finish();
            await(finished, "goAsync finish releases the final result");
            check(code.get() == 8 && "done".equals(data.get()) &&
                  "changed".equals(extras.get().getString("result")), "changed result returned");

            received = new CountDownLatch(1);
            pending.set(null);
            try (ServerSocket listener = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
                 Socket client = new Socket(InetAddress.getLoopbackAddress(), listener.getLocalPort());
                 Socket server = listener.accept()) {
                SharedMicrogServiceRouter router = new SharedMicrogServiceRouter((Context) app.get(null), i -> null);
                BinderBridge serverBridge = new BinderBridge(server.getInputStream(), server.getOutputStream(),
                    router.root(), server::close, 11001, 50001);
                BinderBridge clientBridge = new BinderBridge(client.getInputStream(), client.getOutputStream(),
                    null, client::close);
                SharedMicrogClientRouter clientRouter = new SharedMicrogClientRouter(clientBridge.remoteRoot(),
                    Runnable::run, (i, p, a, h, u, n) -> {});
                try {
                    CountDownLatch roundTrip = new CountDownLatch(1);
                    AtomicInteger remoteCode = new AtomicInteger();
                    AtomicReference<String> remoteData = new AtomicReference<>();
                    AtomicReference<Bundle> remoteExtras = new AtomicReference<>();
                    clientRouter.sendOrderedBroadcast(intent, 3, "initial", null, (c, d, e) -> {
                        remoteCode.set(c); remoteData.set(d); remoteExtras.set(e); roundTrip.countDown();
                    });
                    await(received, "receiver invoked through bridge");
                    check(roundTrip.getCount() == 1, "remote result waits for goAsync");
                    pending.get().setResult(12, "remote", result);
                    pending.get().finish();
                    await(roundTrip, "remote ordered result returned");
                    check(remoteCode.get() == 12 && "remote".equals(remoteData.get()) &&
                          "changed".equals(remoteExtras.get().getString("result")),
                          "ordered result survives the bridge");
                    String[] protectedActions = {
                        "android.intent.action.BOOT_COMPLETED", "android.intent.action.PACKAGE_ADDED",
                        "android.net.conn.CONNECTIVITY_CHANGE", "android.intent.action.SCREEN_ON",
                        "android.intent.action.TIME_SET", "android.intent.action.LOCALE_CHANGED",
                        "android.accounts.LOGIN_ACCOUNTS_CHANGED", "com.google.android.c2dm.intent.RECEIVE",
                        "com.google.android.c2dm.intent.REGISTRATION"
                    };
                    for (String action : protectedActions) {
                        Intent forged = new Intent(action).setComponent(
                            new ComponentName("com.google.android.gms", Receiver.class.getName()));
                        boolean ordinaryRejected = false;
                        try { clientRouter.sendBroadcast(forged); }
                        catch (IllegalStateException expected) { ordinaryRejected = true; }
                        check(ordinaryRejected, "ordinary protected broadcast rejected: " + action);
                        boolean orderedRejected = false;
                        try { clientRouter.sendOrderedBroadcast(forged, 0, null, null, (c, d, e) -> {}); }
                        catch (IllegalStateException expected) { orderedRejected = true; }
                        check(orderedRejected, "explicit protected ordered broadcast rejected: " + action);
                        orderedRejected = false;
                        try {
                            clientRouter.sendOrderedBroadcast(new Intent(action).setPackage("com.google.android.gms"),
                                0, null, null, (c, d, e) -> {});
                        } catch (IllegalStateException expected) { orderedRejected = true; }
                        check(orderedRejected, "implicit protected ordered broadcast rejected: " + action);
                    }
                    Manifest.Component high = implicitReceiver(HighReceiver.class, 50, false);
                    Manifest.Component abort = implicitReceiver(AbortReceiver.class, 10, false);
                    Manifest.Component skipped = implicitReceiver(SkippedReceiver.class, -10, false);
                    Manifest.Component mime = implicitReceiver(SkippedReceiver.class, 100, true);
                    Manifest.receivers.add(skipped);
                    Manifest.receivers.add(abort);
                    Manifest.receivers.add(mime);
                    Manifest.receivers.add(high);
                    CountDownLatch implicitDone = new CountDownLatch(1);
                    AtomicInteger implicitCode = new AtomicInteger();
                    Intent implicit = new Intent("fixture.implicit").setPackage("com.google.android.gms");
                    clientRouter.sendOrderedBroadcast(implicit, 3, null, null, (c, d, e) -> {
                        implicitCode.set(c); implicitDone.countDown();
                    });
                    await(implicitDone, "implicit ordered broadcast completed");
                    check("HA".equals(order.get()) && implicitCode.get() == 5,
                          "matching receivers run by priority and abort prevents later delivery");
                    BroadcastReceiver dynamic = new SkippedReceiver();
                    Broadcasts.register((Context) app.get(null), dynamic,
                        new android.content.IntentFilter("fixture.implicit"));
                    boolean rejected = false;
                    try { clientRouter.sendOrderedBroadcast(implicit, 0, null, null, (c, d, e) -> {}); }
                    catch (IllegalStateException expected) { rejected = true; }
                    finally { Broadcasts.unregister(dynamic); }
                    check(rejected && "HA".equals(order.get()), "unknown dynamic receiver visibility fails closed");
                    order.set("");
                    android.content.IntentFilter exportedFilter = new android.content.IntentFilter("fixture.implicit");
                    exportedFilter.setPriority(75);
                    BroadcastReceiver exported = new HighReceiver();
                    BroadcastReceiver privateReceiver = new SkippedReceiver();
                    BroadcastReceiver protectedReceiver = new SkippedReceiver();
                    Context clientContext = (Context) app.get(null);
                    clientContext.registerReceiver(exported, exportedFilter, Context.RECEIVER_EXPORTED);
                    clientContext.registerReceiver(privateReceiver, exportedFilter, Context.RECEIVER_NOT_EXPORTED);
                    clientContext.registerReceiver(protectedReceiver, exportedFilter, "fixture.permission", null,
                        Context.RECEIVER_EXPORTED);
                    try {
                        CountDownLatch dynamicDone = new CountDownLatch(1);
                        AtomicInteger dynamicCode = new AtomicInteger();
                        clientRouter.sendOrderedBroadcast(implicit, 3, null, null, (c, d, e) -> {
                            dynamicCode.set(c); dynamicDone.countDown();
                        });
                        await(dynamicDone, "exported dynamic ordered receiver completes");
                        check("HHA".equals(order.get()) && dynamicCode.get() == 6,
                              "dynamic and manifest receivers are ordered and private receivers are excluded");
                    } finally {
                        Broadcasts.unregister(exported);
                        Broadcasts.unregister(privateReceiver);
                        Broadcasts.unregister(protectedReceiver);
                    }
                    for (int constraint = 0; constraint < 2; constraint++) {
                        android.content.IntentFilter constrained = new android.content.IntentFilter("fixture.implicit");
                        if (constraint == 0) constrained.addDataAuthority("example.com", null);
                        else constrained.addDataPath("/secret", 0);
                        constrained = new android.content.IntentFilter(constrained);
                        BroadcastReceiver constrainedReceiver = new SkippedReceiver();
                        clientContext.registerReceiver(constrainedReceiver, constrained, Context.RECEIVER_EXPORTED);
                        try {
                            rejected = false;
                            try { clientRouter.sendOrderedBroadcast(implicit, 0, null, null, (c, d, e) -> {}); }
                            catch (IllegalStateException expected) { rejected = true; }
                            check(rejected, "unknown authority and path constraints fail closed");
                        } finally { Broadcasts.unregister(constrainedReceiver); }
                    }
                    Manifest.Component constrainedManifest = implicitReceiver(SkippedReceiver.class, 100, false);
                    constrainedManifest.filters.get(0).unsupportedDataConstraints = true;
                    Manifest.receivers.add(constrainedManifest);
                    try {
                        rejected = false;
                        try { clientRouter.sendOrderedBroadcast(implicit, 0, null, null, (c, d, e) -> {}); }
                        catch (IllegalStateException expected) { rejected = true; }
                        check(rejected, "unknown manifest data constraints fail closed");
                    } finally { Manifest.receivers.remove(constrainedManifest); }
                    Manifest.receivers.remove(high);
                    Manifest.receivers.remove(abort);
                    Manifest.receivers.remove(skipped);
                    Manifest.receivers.remove(mime);
                    worker.setBoolean(null, false);
                    clientUid.setInt(null, 11001);
                    Native.setPackageName("fixture.client");
                    received = new CountDownLatch(1);
                    pending.set(null);
                    Intent toClient = new Intent("android.intent.action.BOOT_COMPLETED").setComponent(
                        new ComponentName("fixture.client", Receiver.class.getName()));
                    CountDownLatch reverseDone = new CountDownLatch(1);
                    AtomicInteger reverseCode = new AtomicInteger();
                    router.broadcastOrderedToClient(toClient, 3, "initial", null, (c, d, e) -> {
                        reverseCode.set(c); reverseDone.countDown();
                    });
                    await(received, "worker ordered broadcast reaches the client");
                    check(reverseDone.getCount() == 1, "worker waits for client goAsync");
                    pending.get().setResultCode(14);
                    pending.get().finish();
                    await(reverseDone, "client ordered result reaches worker");
                    check(reverseCode.get() == 14, "worker receives client result");
                    SharedMicrogConnections connections = new SharedMicrogConnections((Context) app.get(null), id -> {});
                    Field entriesField = SharedMicrogConnections.class.getDeclaredField("connections");
                    entriesField.setAccessible(true);
                    @SuppressWarnings("unchecked")
                    java.util.Map<Long, Object> entries = (java.util.Map<Long, Object>) entriesField.get(connections);
                    Class<?> connectionClass = Class.forName("megler.SharedMicrogConnections$Connection");
                    java.lang.reflect.Constructor<?> connection = connectionClass.getDeclaredConstructor(long.class,
                        String.class, android.os.ParcelFileDescriptor.class, SharedMicrogServiceRouter.class, BinderBridge.class);
                    connection.setAccessible(true);
                    try {
                        entries.put(1L, connection.newInstance(1L, "fixture.client", null, router, serverBridge));
                        received = new CountDownLatch(1);
                        CountDownLatch routedDone = new CountDownLatch(1);
                        SharedMicrogConnections.sendOrderedBroadcastToClient(toClient, 3, "initial", null,
                            (c, d, e) -> routedDone.countDown());
                        await(received, "single client channel is selected");
                        pending.get().finish();
                        await(routedDone, "selected channel returns its ordered result");
                        entries.put(2L, connection.newInstance(2L, "fixture.client", null, router, serverBridge));
                        boolean multipleRejected = false;
                        try { SharedMicrogConnections.sendOrderedBroadcastToClient(toClient, 3, "initial", null, (c, d, e) -> {}); }
                        catch (UnsupportedOperationException expected) { multipleRejected = true; }
                        check(multipleRejected, "ambiguous client channels are rejected");
                    } finally {
                        entries.clear();
                        connections.close();
                    }
                    Native.setPackageName("com.google.android.gms");
                    clientUid.setInt(null, -1);
                    worker.setBoolean(null, true);
                } finally {
                    clientRouter.close();
                    clientBridge.close();
                    serverBridge.close();
                    clientBridge.awaitClosed();
                    serverBridge.awaitClosed();
                    router.close();
                }
            }

            component.permission = "fixture.permission";
            CountDownLatch protectedFinished = new CountDownLatch(1);
            Broadcasts.sendOrderedFromSharedClient(intent, 3, "initial", null,
                (c, d, e) -> protectedFinished.countDown());
            await(protectedFinished, "protected receiver is skipped");
            check(calls.get() == 4, "permission-protected receiver was not invoked");
            component.permission = null;
            component.exported = false;
            CountDownLatch privateFinished = new CountDownLatch(1);
            Broadcasts.sendOrderedFromSharedClient(intent, 3, "initial", null,
                (c, d, e) -> privateFinished.countDown());
            await(privateFinished, "private receiver is skipped");
            check(calls.get() == 4, "non-exported receiver was not invoked");
            Manifest.receivers.remove(component);
            System.out.println("SharedMicrogOrderedDispatchTest: PASS ordered goAsync and receiver access");
        } finally {
            app.set(null, previousApp);
            read.setBoolean(null, previousRead);
            worker.setBoolean(null, previousWorker);
            clientUid.setInt(null, previousUid);
            Native.setPackageName("com.google.android.gms");
            Looper.getMainLooper().quit();
            main.join(5000);
            check(!main.isAlive(), "main looper stopped");
        }
    }
}
