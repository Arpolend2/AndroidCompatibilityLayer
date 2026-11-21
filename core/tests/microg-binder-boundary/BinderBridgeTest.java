import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import megler.BinderBridge;
import java.util.NoSuchElementException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public class BinderBridgeTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void child() throws Exception {
        CountDownLatch firstStarted = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch completed = new CountDownLatch(4);
        CountDownLatch otherTarget = new CountDownLatch(1);
        StringBuilder order = new StringBuilder();
        Binder other = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                check(code == 1 && flags == IBinder.FLAG_ONEWAY && reply == null, "other target dispatch");
                check(Binder.getCallingUid() == 13001 && Binder.getCallingPid() == 7001, "one-way caller identity");
                otherTarget.countDown();
                return true;
            }
        };
        Binder service = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                check(Binder.getCallingUid() == 13001 && Binder.getCallingPid() == 7001, "server caller identity");
                if (code == 50) {
                    check(flags == IBinder.FLAG_ONEWAY && reply == null, "one-way dispatch shape");
                    int value = data.readInt();
                    if (value == 1) {
                        firstStarted.countDown();
                        try {
                            if (!release.await(10, TimeUnit.SECONDS)) throw new RemoteException("release timeout");
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new RemoteException("interrupted");
                        }
                    }
                    if (value == 4) throw new IllegalStateException("synthetic one-way failure");
                    synchronized (order) { order.append(value); }
                    completed.countDown();
                    return true;
                }
                if (code == 51) { release.countDown(); return true; }
                if (code == 52) {
                    try {
                        check(completed.await(10, TimeUnit.SECONDS), "one-way completion");
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new RemoteException("interrupted");
                    }
                    synchronized (order) { reply.writeString(order.toString()); }
                    return true;
                }
                if (code == 53) {
                    try { check(firstStarted.await(5, TimeUnit.SECONDS), "first one-way started"); }
                    catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new RemoteException("interrupted");
                    }
                    return true;
                }
                if (code == 54) { reply.writeStrongBinder(other); return true; }
                if (code == 55) {
                    try { check(otherTarget.await(5, TimeUnit.SECONDS), "other target not blocked"); }
                    catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new RemoteException("interrupted");
                    }
                    return true;
                }
                if (code == 47) throw new SecurityException("synthetic rejection");
                if (code != 46) return false;
                long identity = Binder.clearCallingIdentity();
                check(Binder.getCallingUid() == android.os.Process.myUid() &&
                      Binder.getCallingPid() == android.os.Process.myPid(), "clear caller identity");
                long inner = Binder.clearCallingIdentity();
                Binder.restoreCallingIdentity(inner);
                Binder.restoreCallingIdentity(identity);
                check(Binder.getCallingUid() == 13001 && Binder.getCallingPid() == 7001, "restore caller identity");
                data.enforceInterface("example.service");
                check(data.readInt() == 39, "service request");
                IBinder callback = data.readStrongBinder();
                Parcel request = Parcel.obtain(), result = Parcel.obtain();
                try {
                    request.writeInterfaceToken("example.callback");
                    request.writeString("synthetic-result");
                    check(callback.transact(1, request, result, 0), "callback transaction");
                    check(result.readInt() == 17, "callback reply");
                    check(Binder.getCallingUid() == 13001 && Binder.getCallingPid() == 7001, "caller after nested callback");
                } finally { request.recycle(); result.recycle(); }
                reply.writeStrongBinder(callback);
                reply.writeInt(123);
                return true;
            }
        };
        BinderBridge bridge = new BinderBridge(System.in, System.out, service, null, 13001, 7001);
        bridge.awaitClosed();
    }

    private static void overflow(String javaExe, boolean byBytes) throws Exception {
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                            BinderBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null, null, 13002, 7002);
        try {
            IBinder root = bridge.remoteRoot();
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInt(1);
                if (byBytes) data.writeByteArray(new byte[900000]);
                root.transact(50, data, null, IBinder.FLAG_ONEWAY);
                check(root.transact(53, data, reply, 0), "overflow handler started");
                for (int i = 0; i < (byBytes ? 6 : 140); i++) {
                    try { root.transact(50, data, null, IBinder.FLAG_ONEWAY); }
                    catch (RemoteException disconnected) { break; }
                }
                CountDownLatch closed = new CountDownLatch(1);
                Thread waiter = new Thread(() -> {
                    try { bridge.awaitClosed(); closed.countDown(); }
                    catch (InterruptedException e) { Thread.currentThread().interrupt(); }
                });
                waiter.setDaemon(true);
                waiter.start();
                check(closed.await(5, TimeUnit.SECONDS), "one-way overflow closes connection");
                boolean rejected = false;
                try { root.transact(50, data, null, IBinder.FLAG_ONEWAY); }
                catch (RemoteException expected) { rejected = true; }
                check(rejected, "one-way rejected after disconnect");
            } finally { data.recycle(); reply.recycle(); }
        } finally {
            bridge.close();
            boolean exited = child.waitFor(5, TimeUnit.SECONDS);
            if (!exited) child.destroyForcibly();
            check(exited && child.exitValue() == 0, "overflow child exited");
        }
    }

    private static void deathAndReconnect(String javaExe) throws Exception {
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                            BinderBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        IBinder root = bridge.remoteRoot();
        CountDownLatch died = new CountDownLatch(3);
        AtomicInteger delivered = new AtomicInteger(), removed = new AtomicInteger();
        IBinder.DeathRecipient recipient = new IBinder.DeathRecipient() {
            public void binderDied() { throw new AssertionError("binder identity not supplied"); }
            public void binderDied(IBinder who) {
                if (who != root || who.isBinderAlive()) throw new AssertionError("death identity or liveness");
                delivered.incrementAndGet();
                died.countDown();
                bridge.close();
            }
        };
        IBinder.DeathRecipient unlinked = () -> removed.incrementAndGet();
        IBinder.DeathRecipient throwing = () -> { throw new IllegalStateException("synthetic death failure"); };
        CountDownLatch callbackDied = new CountDownLatch(1);
        IInterface listener = () -> root;
        RemoteCallbackList<IInterface> list = new RemoteCallbackList<IInterface>() {
            @Override public void onCallbackDied(IInterface callback, Object cookie) {
                if (callback == listener && "cookie".equals(cookie)) callbackDied.countDown();
            }
        };
        try {
            check(list.register(listener, "cookie"), "register remote callback");
            root.linkToDeath(recipient, 0);
            root.linkToDeath(recipient, 0);
            check(root.unlinkToDeath(recipient, 0), "remove one duplicate recipient");
            root.linkToDeath(unlinked, 0);
            check(root.unlinkToDeath(unlinked, 0), "unlink registered recipient");
            boolean missing = false;
            try { root.unlinkToDeath(unlinked, 0); }
            catch (NoSuchElementException expected) { missing = true; }
            check(missing, "missing live recipient rejected");
            root.linkToDeath(throwing, 0);
            root.linkToDeath(recipient, 0);
            root.linkToDeath(recipient, 0);
            child.destroyForcibly();
            check(child.waitFor(5, TimeUnit.SECONDS), "terminate peer");
            check(died.await(5, TimeUnit.SECONDS), "all death registrations notified");
            check(callbackDied.await(5, TimeUnit.SECONDS) && list.getRegisteredCallbackCount() == 0,
                  "remote callback list removes dead binder");
            check(delivered.get() == 3 && removed.get() == 0, "death notification counts");
            check(!root.pingBinder() && !root.isBinderAlive(), "dead binder state");
            check(!root.unlinkToDeath(recipient, 0), "unlink after death");
            boolean rejected = false;
            try { root.linkToDeath(recipient, 0); }
            catch (RemoteException expected) { rejected = true; }
            check(rejected, "link after death rejected");
        } finally {
            bridge.close();
            if (child.isAlive()) child.destroyForcibly();
        }
        Process restarted = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                                BinderBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        AtomicInteger shutdowns = new AtomicInteger();
        BinderBridge fresh = new BinderBridge(restarted.getInputStream(), restarted.getOutputStream(), null,
                                               shutdowns::incrementAndGet);
        CountDownLatch closed = new CountDownLatch(1);
        try {
            IBinder newRoot = fresh.remoteRoot();
            check(newRoot != root && newRoot.isBinderAlive(), "reconnection creates live proxy");
            newRoot.linkToDeath(closed::countDown, 0);
            fresh.close();
            fresh.close();
            check(closed.await(5, TimeUnit.SECONDS), "local close reports death");
            check(shutdowns.get() == 1, "transport shutdown invoked once");
        } finally {
            fresh.close();
            if (!restarted.waitFor(5, TimeUnit.SECONDS)) restarted.destroyForcibly();
        }
    }

    private static void staleCallbackDeath() {
        class FakeBinder extends Binder {
            IBinder.DeathRecipient recipient;
            @Override public void linkToDeath(IBinder.DeathRecipient value, int flags) { recipient = value; }
            @Override public boolean unlinkToDeath(IBinder.DeathRecipient value, int flags) {
                if (recipient != value) return false;
                recipient = null;
                return true;
            }
        }
        FakeBinder binder = new FakeBinder();
        IInterface listener = () -> binder;
        AtomicInteger died = new AtomicInteger();
        RemoteCallbackList<IInterface> list = new RemoteCallbackList<IInterface>() {
            @Override public void onCallbackDied(IInterface callback, Object cookie) { died.incrementAndGet(); }
        };
        check(list.register(listener, "old"), "register old callback");
        IBinder.DeathRecipient old = binder.recipient;
        check(list.register(listener, "new"), "replace callback");
        old.binderDied();
        check(died.get() == 0 && list.getRegisteredCallbackCount() == 1 &&
              "new".equals(list.getRegisteredCallbackCookie(0)), "stale death preserves replacement");
        check(list.unregister(listener), "unregister replacement");
    }

    private static void deathRace(String javaExe) throws Exception {
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                            BinderBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null);
        IBinder root = bridge.remoteRoot();
        Object progress = new Object();
        AtomicInteger added = new AtomicInteger(1), removed = new AtomicInteger(), calls = new AtomicInteger();
        AtomicBoolean duplicate = new AtomicBoolean();
        IBinder.DeathRecipient first = () -> {
            calls.incrementAndGet();
            synchronized (progress) { progress.notifyAll(); }
        };
        root.linkToDeath(first, 0);
        CountDownLatch started = new CountDownLatch(1);
        Thread linker = new Thread(() -> {
            started.countDown();
            for (int i = 0; i < 1000; i++) {
                AtomicBoolean called = new AtomicBoolean();
                IBinder.DeathRecipient recipient = () -> {
                    if (!called.compareAndSet(false, true)) duplicate.set(true);
                    calls.incrementAndGet();
                    synchronized (progress) { progress.notifyAll(); }
                };
                try {
                    root.linkToDeath(recipient, 0);
                    added.incrementAndGet();
                    if ((i & 1) == 0 && root.unlinkToDeath(recipient, 0)) removed.incrementAndGet();
                } catch (RemoteException closed) { break; }
            }
        });
        try {
            linker.start();
            check(started.await(5, TimeUnit.SECONDS), "link race started");
            bridge.close();
            linker.join(5000);
            check(!linker.isAlive(), "link race completed");
            int expected = added.get() - removed.get();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            synchronized (progress) {
                while (calls.get() < expected) {
                    long left = deadline - System.nanoTime();
                    if (left <= 0) break;
                    TimeUnit.NANOSECONDS.timedWait(progress, left);
                }
            }
            check(calls.get() == expected && !duplicate.get(), "link/unlink race notifies exactly once");
        } finally {
            bridge.close();
            if (!child.waitFor(5, TimeUnit.SECONDS)) child.destroyForcibly();
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && args[0].equals("child")) { child(); return; }
        String javaExe = System.getProperty("java.home") + java.io.File.separator + "bin"
            + java.io.File.separator + "java";
        Process child = new ProcessBuilder(javaExe, "-cp", System.getProperty("java.class.path"),
                                            BinderBridgeTest.class.getName(), "child")
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        BinderBridge bridge = new BinderBridge(child.getInputStream(), child.getOutputStream(), null, null, 13002, 7002);
        try {
            final int[] called = {0};
            Binder callback = new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    check(code == 1, "callback code");
                    check(Binder.getCallingUid() == 13002 && Binder.getCallingPid() == 7002, "callback caller identity");
                    data.enforceInterface("example.callback");
                    check("synthetic-result".equals(data.readString()), "callback payload");
                    called[0]++;
                    reply.writeInt(17);
                    return true;
                }
            };
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken("example.service");
                data.writeInt(39);
                data.writeStrongBinder(callback);
                check(bridge.remoteRoot().transact(46, data, reply, 0), "remote transaction");
                check(called[0] == 1, "callback delivered");
                check(reply.readStrongBinder() == callback, "returned callback identity");
                check(reply.readInt() == 123, "remote reply");
                boolean rejected = false;
                try { bridge.remoteRoot().transact(46, data, reply, 2); }
                catch (RemoteException expected) { rejected = true; }
                check(rejected, "unsupported flags rejected");
                rejected = false;
                try { bridge.remoteRoot().transact(47, data, reply, 0); }
                catch (RemoteException expected) { rejected = true; }
                check(rejected, "remote failure must reach caller");
                check(!bridge.remoteRoot().transact(48, data, reply, 0), "unhandled transaction returns false");
            } finally { data.recycle(); reply.recycle(); }
            IBinder root = bridge.remoteRoot();
            Parcel first = Parcel.obtain(), signal = Parcel.obtain(), result = Parcel.obtain();
            try {
                first.writeInt(1);
                check(root.transact(50, first, null, IBinder.FLAG_ONEWAY), "one-way accepted");
                first.recycle();
                first = null;
                check(root.transact(53, signal, result, 0), "one-way began without blocking caller");
                check(root.transact(54, signal, result, 0), "retrieve other target");
                IBinder other = result.readStrongBinder();
                Parcel unchanged = Parcel.obtain();
                try {
                    unchanged.writeInt(1234);
                    unchanged.setDataPosition(0);
                    check(other.transact(1, signal, unchanged, IBinder.FLAG_ONEWAY), "other target accepted");
                    check(unchanged.readInt() == 1234, "one-way reply untouched");
                } finally { unchanged.recycle(); }
                check(root.transact(55, signal, result, 0), "separate target delivered concurrently");
                for (int value = 2; value <= 5; value++) {
                    Parcel item = Parcel.obtain();
                    try {
                        item.writeInt(value);
                        check(root.transact(50, item, null, IBinder.FLAG_ONEWAY), "queued one-way");
                    } finally { item.recycle(); }
                }
                check(root.transact(51, signal, result, 0), "release one-way");
                check(root.transact(52, signal, result, 0), "wait for one-way delivery");
                check("1235".equals(result.readString()), "one-way ordering and failure isolation");
            } finally {
                if (first != null) first.recycle();
                signal.recycle();
                result.recycle();
            }
        } finally {
            bridge.close();
            boolean exited = child.waitFor(5, TimeUnit.SECONDS);
            if (!exited) child.destroyForcibly();
            check(exited, "child did not exit");
            check(child.exitValue() == 0, "child failed");
        }
        overflow(javaExe, false);
        overflow(javaExe, true);
        deathAndReconnect(javaExe);
        deathRace(javaExe);
        staleCallbackDeath();
        System.out.println("Binder bridge synchronous, one-way and death tests passed");
    }
}
