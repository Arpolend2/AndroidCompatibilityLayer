package megler;

import android.os.IBinder;
import android.os.IInterface;
import android.os.Binder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.NoSuchElementException;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Experimental Binder transport over a private duplex stream.
 * The stream owner must authenticate its peer and close it on revocation.
 */
public final class BinderBridge implements AutoCloseable {
    private static final int REQUEST = 1, RESPONSE = 2;
    private static final int MAX_FRAME = 1100000, MAX_BINDERS = 4096, MAX_PENDING = 128;
    private static final int MAX_ONEWAY = 128, MAX_ONEWAY_BYTES = 4 * 1024 * 1024;
    private static final int MAX_DEATH_RECIPIENTS = 4096;
    private static final int MAX_FDS = 16;
    private static final long TIMEOUT_SECONDS = 15;
    private final DataInputStream input;
    private final DataOutputStream output;
    private final Closeable shutdownTransport;
    private final MarkerTransport markerTransport;
    private final int peerUid, peerPid;
    private final Object state = new Object();
    private final IdentityHashMap<IBinder, Long> localIds = new IdentityHashMap<>();
    private final HashMap<Long, IBinder> local = new HashMap<>();
    private final HashMap<Long, RemoteBinder> remote = new HashMap<>();
    private final HashMap<Long, Pending> pending = new HashMap<>();
    private final HashMap<Long, ArrayDeque<OneWayCall>> oneWayQueues = new HashMap<>();
    private final AtomicLong sequence = new AtomicLong();
    private final CountDownLatch disconnected = new CountDownLatch(1);
    private final ThreadPoolExecutor workers;
    private final ThreadPoolExecutor oneWayWorkers;
    private int oneWayCount, oneWayBytes, deathRecipientCount;
    private volatile boolean closed;
    private long nextBinder = 1;

    private static final class Pending {
        final CountDownLatch done = new CountDownLatch(1);
        volatile int status;
        volatile FramePayload payload;
    }

    private static final class OneWayCall {
        final int code;
        final FramePayload payload;
        OneWayCall(int code, FramePayload payload) {
            this.code = code;
            this.payload = payload;
        }
    }

    interface MarkerTransport {
        void send(int[] descriptors) throws IOException;
        int[] receive() throws IOException;
    }

    private static final class DescriptorBatch implements AutoCloseable {
        private final ArrayList<ParcelFileDescriptor> descriptors = new ArrayList<>();
        private final IdentityHashMap<ParcelFileDescriptor, Boolean> returned = new IdentityHashMap<>();
        private boolean[] used;

        static DescriptorBatch receive(int[] values) {
            if (values == null) return null;
            DescriptorBatch batch = new DescriptorBatch();
            for (int value : values) batch.descriptors.add(ParcelFileDescriptor.adoptFd(value));
            batch.used = new boolean[values.length];
            return batch;
        }

        int add(ParcelFileDescriptor value) {
            if (descriptors.size() >= MAX_FDS) throw new IllegalArgumentException("Binder descriptor limit exceeded");
            try { descriptors.add(value.dup()); }
            catch (IOException e) { throw new IllegalArgumentException("Cannot duplicate Binder descriptor", e); }
            return descriptors.size() - 1;
        }

        int[] numbers() {
            int[] numbers = new int[descriptors.size()];
            for (int i = 0; i < numbers.length; i++) numbers[i] = descriptors.get(i).getFd();
            return numbers;
        }

        void validate(int index) {
            if (used == null || index < 0 || index >= used.length || used[index])
                throw new IllegalArgumentException("Invalid or duplicate Binder descriptor index");
            used[index] = true;
        }

        void finishValidation() {
            if (used != null) for (boolean value : used)
                if (!value) throw new IllegalArgumentException("Unreferenced Binder descriptor");
        }

        ParcelFileDescriptor take(int index) {
            ParcelFileDescriptor value = descriptors.get(index);
            if (value == null) throw new IllegalArgumentException("Binder descriptor already consumed");
            descriptors.set(index, null);
            return value;
        }

        void complete() {
            for (ParcelFileDescriptor value : returned.keySet()) {
                try { value.meglerCloseAfterTransfer(); }
                catch (IOException e) { System.err.println("Binder returned descriptor close failed: " + e.getMessage()); }
            }
            returned.clear();
        }

        @Override public void close() {
            for (ParcelFileDescriptor value : descriptors) {
                if (value != null) try { value.close(); }
                catch (IOException e) { System.err.println("Binder descriptor close failed: " + e.getMessage()); }
            }
            descriptors.clear();
        }
    }

    private static final class FramePayload implements AutoCloseable {
        final byte[] bytes;
        final DescriptorBatch descriptors;
        FramePayload(byte[] bytes, DescriptorBatch descriptors) {
            this.bytes = bytes;
            this.descriptors = descriptors;
        }
        @Override public void close() { descriptors.close(); }
    }

    private static final class DeathNotice {
        final IBinder binder;
        final IBinder.DeathRecipient recipient;
        DeathNotice(IBinder binder, IBinder.DeathRecipient recipient) {
            this.binder = binder;
            this.recipient = recipient;
        }
    }

    public BinderBridge(InputStream in, OutputStream out, IBinder root) {
        this(in, out, root, null);
    }

    /**
     * shutdownTransport must interrupt blocked I/O on the owned transport before
     * the streams close. For a socket, use shutdown(SHUT_RDWR), not just close.
     */
    public BinderBridge(InputStream in, OutputStream out, IBinder root, Closeable shutdownTransport) {
        this(in, out, root, shutdownTransport, -1, 0);
    }

    /**
     * The owner supplies the identity bound to this connection, never data from the peer's stream.
     * Authentication and package-to-UID mapping remain the owner's responsibility.
     */
    public BinderBridge(InputStream in, OutputStream out, IBinder root, Closeable shutdownTransport,
                        int peerUid, int peerPid) {
        this(in, out, root, shutdownTransport, peerUid, peerPid, null);
    }

    static MarkerTransport descriptorTransport(int descriptor) {
        if (descriptor < 0) throw new IllegalArgumentException("Invalid Binder transport descriptor");
        return new MarkerTransport() {
            @Override public void send(int[] fds) throws IOException { SharedMicrogFdTransport.sendMarker(descriptor, fds); }
            @Override public int[] receive() throws IOException { return SharedMicrogFdTransport.receiveMarker(descriptor); }
        };
    }

    BinderBridge(InputStream in, OutputStream out, IBinder root, Closeable shutdownTransport,
                 int peerUid, int peerPid, MarkerTransport markerTransport) {
        if (in == null || out == null) throw new IllegalArgumentException("Duplex streams required");
        if ((peerUid < 0 || peerPid <= 0) && (peerUid != -1 || peerPid != 0))
            throw new IllegalArgumentException("Invalid peer identity");
        input = new DataInputStream(in);
        output = new DataOutputStream(out);
        this.shutdownTransport = shutdownTransport;
        this.markerTransport = markerTransport;
        this.peerUid = peerUid;
        this.peerPid = peerPid;
        if (root != null) register(root);
        workers = new ThreadPoolExecutor(8, 8, 0, TimeUnit.SECONDS,
            new ArrayBlockingQueue<Runnable>(32), r -> {
                Thread thread = new Thread(r, "megler-binder-dispatch");
                thread.setDaemon(true);
                return thread;
            });
        oneWayWorkers = new ThreadPoolExecutor(8, 8, 0, TimeUnit.SECONDS,
            new ArrayBlockingQueue<Runnable>(32), r -> {
                Thread thread = new Thread(r, "megler-binder-oneway");
                thread.setDaemon(true);
                return thread;
            });
        Thread reader = new Thread(this::readLoop, "megler-binder-reader");
        reader.setDaemon(true);
        reader.start();
    }

    /** The peer's root object is handle 1. */
    public IBinder remoteRoot() {
        synchronized (state) {
            if (closed) throw new IllegalStateException("Binder bridge is closed");
            return proxy(1);
        }
    }

    private long register(IBinder binder) {
        synchronized (state) {
            Long id = localIds.get(binder);
            if (id != null) return id;
            if (closed || local.size() >= MAX_BINDERS) throw new IllegalStateException("Binder registry unavailable");
            id = nextBinder++;
            localIds.put(binder, id);
            local.put(id, binder);
            return id;
        }
    }

    private long exportBinder(IBinder binder) {
        if (binder instanceof RemoteBinder) {
            RemoteBinder proxy = (RemoteBinder) binder;
            if (proxy.bridge != this) throw new IllegalArgumentException("Binder belongs to another bridge");
            return proxy.id * 2;
        }
        return register(binder) * 2 + 1;
    }

    private IBinder importBinder(long handle) {
        long id = handle / 2;
        if (id < 1) return null;
        synchronized (state) {
            if (closed) return null;
            if ((handle & 1) == 0) return local.get(id);
            return proxy(id);
        }
    }

    private RemoteBinder proxy(long id) {
        RemoteBinder binder = remote.get(id);
        if (binder == null) {
            if (remote.size() >= MAX_BINDERS) throw new IllegalArgumentException("Too many remote binders");
            binder = new RemoteBinder(id);
            remote.put(id, binder);
        }
        return binder;
    }

    private void send(byte[] frame, DescriptorBatch descriptors) throws IOException {
        if (frame.length > MAX_FRAME) throw new IOException("Binder frame too large");
        synchronized (output) {
            if (closed) throw new IOException("Binder bridge closed");
            if (markerTransport != null) markerTransport.send(descriptors.numbers());
            else if (!descriptors.descriptors.isEmpty()) throw new IOException("Binder descriptor transport unavailable");
            output.writeInt(frame.length);
            output.write(frame);
            output.flush();
            if (!descriptors.descriptors.isEmpty())
                System.out.println("Shared microG Binder descriptors sent");
            descriptors.complete();
        }
    }

    private void respond(long request, int status, FramePayload payload) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream frame = new DataOutputStream(bytes);
        frame.writeByte(RESPONSE);
        frame.writeLong(request);
        frame.writeInt(status);
        frame.writeInt(payload.bytes.length);
        frame.write(payload.bytes);
        send(bytes.toByteArray(), payload.descriptors);
    }

    private FramePayload encode(Parcel parcel) {
        DescriptorBatch descriptors = new DescriptorBatch();
        try {
            byte[] bytes = parcel.meglerMarshallBinders(new Parcel.MeglerBinderExporter() {
                @Override public long exportBinder(IBinder binder) { return BinderBridge.this.exportBinder(binder); }
                @Override public int exportFileDescriptor(ParcelFileDescriptor fd) {
                    if (markerTransport == null) throw new IllegalArgumentException("Binder descriptor transport unavailable");
                    return descriptors.add(fd);
                }
                @Override public void exportReturningFileDescriptor(ParcelFileDescriptor fd) {
                    if (markerTransport == null) throw new IllegalArgumentException("Binder descriptor transport unavailable");
                    descriptors.returned.put(fd, Boolean.TRUE);
                }
            });
            return new FramePayload(bytes, descriptors);
        } catch (RuntimeException | Error failure) {
            descriptors.close();
            throw failure;
        }
    }

    private Parcel decode(FramePayload payload) {
        DescriptorBatch descriptors = payload.descriptors;
        Parcel result = Parcel.meglerUnmarshallBinders(payload.bytes, new Parcel.MeglerBinderImporter() {
            @Override public IBinder importBinder(long handle) { return BinderBridge.this.importBinder(handle); }
            @Override public void validateFileDescriptor(int index) { descriptors.validate(index); }
            @Override public void finishValidation() { descriptors.finishValidation(); }
            @Override public ParcelFileDescriptor importFileDescriptor(int index) { return descriptors.take(index); }
        });
        if (!descriptors.descriptors.isEmpty())
            System.out.println("Shared microG Binder descriptors received");
        return result;
    }

    private static FramePayload emptyPayload() {
        return new FramePayload(new byte[0], new DescriptorBatch());
    }

    private void readLoop() {
        try {
            while (!closed) {
                DescriptorBatch descriptors = markerTransport == null ? new DescriptorBatch() :
                    DescriptorBatch.receive(markerTransport.receive());
                if (descriptors == null) throw new java.io.EOFException();
                boolean transferred = false;
                try {
                    int size = input.readInt();
                    if (size < 1 || size > MAX_FRAME) throw new IOException("Invalid Binder frame size");
                    byte[] bytes = new byte[size];
                    input.readFully(bytes);
                    DataInputStream frame = new DataInputStream(new ByteArrayInputStream(bytes));
                    int kind = frame.readUnsignedByte();
                    long request = frame.readLong();
                    if (request <= 0) throw new IOException("Invalid Binder request id");
                    if (kind == RESPONSE) {
                        int status = frame.readInt(), length = frame.readInt();
                        if (status < 0 || status > 2 || length < 0 || length != frame.available() ||
                                (status == 2 && !descriptors.descriptors.isEmpty()))
                            throw new IOException("Invalid Binder response");
                        byte[] payload = new byte[length];
                        frame.readFully(payload);
                        synchronized (state) {
                            Pending call = pending.remove(request);
                            if (call != null) {
                                call.status = status;
                                call.payload = new FramePayload(payload, descriptors);
                                transferred = true;
                                call.done.countDown();
                            }
                        }
                    } else if (kind == REQUEST) {
                        long target = frame.readLong();
                        int code = frame.readInt(), flags = frame.readInt(), length = frame.readInt();
                        if (target <= 0 || (flags != 0 && flags != IBinder.FLAG_ONEWAY) ||
                            length < 0 || length != frame.available())
                            throw new IOException("Invalid Binder request");
                        byte[] payload = new byte[length];
                        frame.readFully(payload);
                        FramePayload owned = new FramePayload(payload, descriptors);
                        if (flags == IBinder.FLAG_ONEWAY) {
                            enqueueOneWay(target, code, owned);
                            transferred = true;
                        } else {
                            try {
                                workers.execute(new DispatchTask(request, target, code, owned));
                                transferred = true;
                            } catch (java.util.concurrent.RejectedExecutionException e) {
                                try (FramePayload empty = emptyPayload()) { respond(request, 2, empty); }
                            }
                        }
                    } else throw new IOException("Unknown Binder frame type");
                } finally {
                    if (!transferred) descriptors.close();
                }
            }
        } catch (java.io.EOFException e) {
            // The peer closed its stream; pending callers are failed by close().
        } catch (IOException e) {
            if (!closed) System.err.println("Binder bridge disconnected: " + e.getMessage());
        } finally {
            close();
        }
    }

    private void enqueueOneWay(long target, int code, FramePayload payload) throws IOException {
        synchronized (state) {
            if (closed) throw new IOException("Binder bridge closed");
            if (oneWayCount >= MAX_ONEWAY || payload.bytes.length > MAX_ONEWAY_BYTES - oneWayBytes)
                throw new IOException("Binder one-way queue limit exceeded");
            ArrayDeque<OneWayCall> queue = oneWayQueues.get(target);
            if (queue == null) {
                queue = new ArrayDeque<>();
                oneWayQueues.put(target, queue);
                try {
                    oneWayWorkers.execute(() -> drainOneWay(target));
                } catch (java.util.concurrent.RejectedExecutionException e) {
                    oneWayQueues.remove(target);
                    throw new IOException("Binder one-way dispatcher unavailable", e);
                }
            }
            queue.addLast(new OneWayCall(code, payload));
            ++oneWayCount;
            oneWayBytes += payload.bytes.length;
        }
    }

    private void drainOneWay(long target) {
        for (;;) {
            OneWayCall call;
            synchronized (state) {
                if (closed) return;
                ArrayDeque<OneWayCall> queue = oneWayQueues.get(target);
                call = queue.pollFirst();
                if (call == null) {
                    oneWayQueues.remove(target);
                    return;
                }
            }
            try {
                dispatchOneWay(target, call);
            } catch (Error error) {
                close();
                throw error;
            } finally {
                synchronized (state) {
                    --oneWayCount;
                    oneWayBytes -= call.payload.bytes.length;
                }
            }
        }
    }

    private void dispatchOneWay(long target, OneWayCall call) {
        Parcel data = null;
        try {
            IBinder binder;
            synchronized (state) { binder = local.get(target); }
            if (binder == null) throw new IllegalArgumentException("Unknown Binder target");
            data = decode(call.payload);
            transactLocal(binder, call.code, data, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException | RuntimeException e) {
            System.err.println("Binder one-way dispatch failed: " + e.getClass().getName());
        } finally {
            if (data != null) data.recycle();
            call.payload.close();
        }
    }

    private final class DispatchTask implements Runnable {
        final long request, target;
        final int code;
        final FramePayload payload;
        DispatchTask(long request, long target, int code, FramePayload payload) {
            this.request = request; this.target = target; this.code = code; this.payload = payload;
        }
        @Override public void run() { dispatch(request, target, code, payload); }
    }

    private void dispatch(long request, long target, int code, FramePayload payload) {
        Parcel data = null, reply = Parcel.obtain();
        reply.meglerEnableDeferredFdReturns();
        try {
            IBinder binder;
            synchronized (state) { binder = local.get(target); }
            if (binder == null) throw new IllegalArgumentException("Unknown Binder target");
            data = decode(payload);
            boolean result = transactLocal(binder, code, data, reply, 0);
            try (FramePayload encoded = encode(reply)) { respond(request, result ? 1 : 0, encoded); }
        } catch (IOException e) {
            System.err.println("Binder response failed: " + e);
            close();
        } catch (RemoteException | RuntimeException e) {
            System.err.println("Binder dispatch failed: " + e);
            e.printStackTrace();
            try (FramePayload empty = emptyPayload()) { respond(request, 2, empty); }
            catch (IOException sendError) { close(); }
        } finally {
            if (data != null) data.recycle();
            reply.recycle();
            payload.close();
        }
    }

    private boolean transactLocal(IBinder binder, int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (peerUid < 0) return binder.transact(code, data, reply, flags);
        return Binder.meglerTransactWithCaller(binder, code, data, reply, flags, peerUid, peerPid);
    }

    private boolean transact(long target, int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        boolean oneWay = flags == IBinder.FLAG_ONEWAY;
        if ((flags != 0 && !oneWay) || data == null || (!oneWay && reply == null))
            throw new RemoteException("Unsupported Binder flags or missing parcel");
        final FramePayload payload;
        try { payload = encode(data); }
        catch (RuntimeException e) { throw new RemoteException("Unsupported Binder parcel: " + e.getMessage()); }
        long id = sequence.incrementAndGet();
        if (id <= 0) {
            payload.close();
            throw new RemoteException("Binder request ids exhausted");
        }
        Pending call = oneWay ? null : new Pending();
        synchronized (state) {
            if (closed || (!oneWay && pending.size() >= MAX_PENDING)) {
                payload.close();
                throw new RemoteException("Binder bridge unavailable");
            }
            if (!oneWay) pending.put(id, call);
        }
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream frame = new DataOutputStream(bytes);
            frame.writeByte(REQUEST);
            frame.writeLong(id);
            frame.writeLong(target);
            frame.writeInt(code);
            frame.writeInt(flags);
            frame.writeInt(payload.bytes.length);
            frame.write(payload.bytes);
            send(bytes.toByteArray(), payload.descriptors);
            if (oneWay) return true;
            if (!call.done.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))
                throw new RemoteException("Binder transaction timed out");
            if (call.payload == null || call.status == 2) throw new RemoteException("Remote Binder transaction failed");
            Parcel received;
            try { received = decode(call.payload); }
            catch (RuntimeException e) { throw new RemoteException("Invalid Binder reply: " + e.getMessage()); }
            try {
                reply.setDataSize(0);
                reply.setDataPosition(0);
                reply.appendFrom(received, 0, received.dataSize());
                reply.setDataPosition(0);
            } finally { received.recycle(); }
            return call.status == 1;
        } catch (IOException e) {
            close();
            throw new RemoteException("Binder connection failed");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new RemoteException("Binder transaction interrupted");
        } finally {
            payload.close();
            if (!oneWay) {
                FramePayload received;
                synchronized (state) {
                    pending.remove(id);
                    received = call.payload;
                    call.payload = null;
                }
                if (received != null) received.close();
            }
        }
    }

    @Override public void close() {
        final ArrayList<DeathNotice> notices = new ArrayList<>();
        synchronized (state) {
            if (closed) return;
            closed = true;
            for (Pending call : pending.values()) call.done.countDown();
            local.clear();
            localIds.clear();
            for (RemoteBinder binder : remote.values()) {
                for (IBinder.DeathRecipient recipient : binder.deathRecipients)
                    notices.add(new DeathNotice(binder, recipient));
                binder.deathRecipients.clear();
            }
            deathRecipientCount = 0;
            remote.clear();
            for (ArrayDeque<OneWayCall> queue : oneWayQueues.values())
                for (OneWayCall call : queue) call.payload.close();
            oneWayQueues.clear();
        }
        if (!notices.isEmpty()) {
            Thread notifier = new Thread(() -> {
                for (DeathNotice notice : notices) {
                    try { notice.recipient.binderDied(notice.binder); }
                    catch (RuntimeException e) {
                        System.err.println("Binder death recipient failed: " + e.getClass().getName());
                    }
                }
            }, "megler-binder-death");
            notifier.setDaemon(true);
            notifier.start();
        }
        for (Runnable task : workers.shutdownNow())
            if (task instanceof BinderBridge.DispatchTask) ((DispatchTask) task).payload.close();
        oneWayWorkers.shutdownNow();
        if (shutdownTransport != null) {
            try { shutdownTransport.close(); }
            catch (IOException | RuntimeException e) {
                System.err.println("Binder transport shutdown failed: " + e);
            }
        }
        try { output.close(); } catch (IOException e) { System.err.println("Binder output close failed: " + e.getMessage()); }
        try { input.close(); } catch (IOException e) { System.err.println("Binder input close failed: " + e.getMessage()); }
        disconnected.countDown();
    }

    public void awaitClosed() throws InterruptedException { disconnected.await(); }

    private final class RemoteBinder implements IBinder {
        final BinderBridge bridge = BinderBridge.this;
        final long id;
        final ArrayList<DeathRecipient> deathRecipients = new ArrayList<>();
        RemoteBinder(long id) { this.id = id; }
        @Override public boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            return bridge.transact(id, code, data, reply, flags);
        }
        @Override public String getInterfaceDescriptor() throws RemoteException {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                if (!transact(INTERFACE_TRANSACTION, data, reply, 0))
                    throw new RemoteException("Binder descriptor unavailable");
                return reply.readString();
            } finally { data.recycle(); reply.recycle(); }
        }
        @Override public boolean pingBinder() { return !closed; }
        @Override public boolean isBinderAlive() { return !closed; }
        @Override public IInterface queryLocalInterface(String descriptor) { return null; }
        @Override public void dump(java.io.FileDescriptor fd, String[] args) throws RemoteException {
            throw new RemoteException("Binder dump is unsupported");
        }
        @Override public void dumpAsync(java.io.FileDescriptor fd, String[] args) throws RemoteException {
            throw new RemoteException("Binder dump is unsupported");
        }
        @Override public void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException {
            if (recipient == null || flags != 0) throw new IllegalArgumentException("Invalid death recipient or flags");
            synchronized (state) {
                if (closed) throw new RemoteException("Binder bridge closed");
                if (deathRecipientCount >= MAX_DEATH_RECIPIENTS)
                    throw new RemoteException("Binder death recipient limit exceeded");
                deathRecipients.add(recipient);
                ++deathRecipientCount;
            }
        }
        @Override public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
            if (recipient == null || flags != 0) throw new IllegalArgumentException("Invalid death recipient or flags");
            synchronized (state) {
                if (closed) return false;
                for (int i = 0; i < deathRecipients.size(); i++) {
                    if (deathRecipients.get(i) == recipient) {
                        deathRecipients.remove(i);
                        --deathRecipientCount;
                        return true;
                    }
                }
                throw new NoSuchElementException("Death recipient not registered");
            }
        }
    }
}
