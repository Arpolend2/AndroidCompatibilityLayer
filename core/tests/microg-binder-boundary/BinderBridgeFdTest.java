package megler;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public final class BinderBridgeFdTest {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static final class Marker implements BinderBridge.MarkerTransport {
        final InputStream input;
        final OutputStream output;
        final LinkedBlockingQueue<int[]> incoming;
        final LinkedBlockingQueue<int[]> outgoing;
        Marker(InputStream input, OutputStream output, LinkedBlockingQueue<int[]> incoming,
               LinkedBlockingQueue<int[]> outgoing) {
            this.input = input; this.output = output; this.incoming = incoming; this.outgoing = outgoing;
        }
        @Override public void send(int[] descriptors) throws java.io.IOException {
            int[] copies = new int[descriptors.length];
            int count = 0;
            try {
                for (; count < descriptors.length; count++)
                    copies[count] = ParcelFileDescriptor.fromFd(descriptors[count]).detachFd();
                outgoing.add(copies);
                output.write(0);
                output.flush();
            } catch (java.io.IOException | RuntimeException e) {
                for (int i = 0; i < count; i++) ParcelFileDescriptor.adoptFd(copies[i]).close();
                throw e;
            }
        }
        @Override public int[] receive() throws java.io.IOException {
            if (input.read() < 0) return null;
            try {
                int[] values = incoming.poll(5, TimeUnit.SECONDS);
                if (values == null) throw new java.io.IOException("Missing marker");
                return values;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new java.io.IOException(e);
            }
        }
    }

    private static void invalidWireDoesNotImport() throws Exception {
        ParcelFileDescriptor[] pair = ParcelFileDescriptor.createSocketPair();
        Parcel data = Parcel.obtain();
        try {
            data.writeFileDescriptor(pair[0].getFileDescriptor());
            data.writeFileDescriptor(pair[1].getFileDescriptor());
            byte[] wire = data.meglerMarshallBinders(new Parcel.MeglerBinderExporter() {
                @Override public long exportBinder(IBinder binder) { throw new AssertionError(); }
                @Override public int exportFileDescriptor(ParcelFileDescriptor fd) { return 0; }
            });
            final int[] seen = {0, 0};
            Parcel.MeglerBinderImporter importer = new Parcel.MeglerBinderImporter() {
                @Override public IBinder importBinder(long handle) { throw new AssertionError(); }
                @Override public void validateFileDescriptor(int index) {
                    if (index != 0 || seen[0]++ != 0) throw new IllegalArgumentException("duplicate descriptor");
                }
                @Override public ParcelFileDescriptor importFileDescriptor(int index) {
                    seen[1]++;
                    throw new AssertionError("descriptor imported before validation");
                }
            };
            try { Parcel.meglerUnmarshallBinders(wire, importer); throw new AssertionError("duplicate accepted"); }
            catch (IllegalArgumentException expected) { check(seen[1] == 0, "partial import"); }
        } finally {
            data.recycle(); pair[0].close(); pair[1].close();
        }
    }

    private static void failureKeepsReturnedDescriptor() throws Exception {
        try (ServerSocket listener = new ServerSocket(0);
             Socket clientSocket = new Socket("127.0.0.1", listener.getLocalPort());
             Socket peer = listener.accept()) {
            BinderBridge.MarkerTransport failing = new BinderBridge.MarkerTransport() {
                @Override public void send(int[] descriptors) throws java.io.IOException {
                    throw new java.io.IOException("injected send failure");
                }
                @Override public int[] receive() throws java.io.IOException {
                    return clientSocket.getInputStream().read() < 0 ? null : new int[0];
                }
            };
            BinderBridge bridge = new BinderBridge(clientSocket.getInputStream(), clientSocket.getOutputStream(),
                null, clientSocket::close, -1, 0, failing);
            ParcelFileDescriptor[] pair = ParcelFileDescriptor.createSocketPair();
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.meglerEnableDeferredFdReturns();
                pair[0].writeToParcel(data, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                try { bridge.remoteRoot().transact(1, data, reply, 0); throw new AssertionError("send succeeded"); }
                catch (android.os.RemoteException expected) {}
                check(pair[0].getFileDescriptor().valid(), "failed return closed original");
            } finally {
                bridge.close(); bridge.awaitClosed();
                data.recycle(); reply.recycle(); pair[0].close(); pair[1].close();
            }
        }
    }

    private static void invalidFrameClosesDescriptors() throws Exception {
        LinkedBlockingQueue<int[]> toServer = new LinkedBlockingQueue<>(), toClient = new LinkedBlockingQueue<>();
        try (ServerSocket listener = new ServerSocket(0);
             Socket clientSocket = new Socket("127.0.0.1", listener.getLocalPort());
             Socket serverSocket = listener.accept()) {
            Marker sender = new Marker(clientSocket.getInputStream(), clientSocket.getOutputStream(), toClient, toServer);
            Marker receiver = new Marker(serverSocket.getInputStream(), serverSocket.getOutputStream(), toServer, toClient);
            BinderBridge bridge = new BinderBridge(serverSocket.getInputStream(), serverSocket.getOutputStream(),
                null, serverSocket::close, -1, 0, receiver);
            ParcelFileDescriptor[] pair = ParcelFileDescriptor.createSocketPair();
            try {
                sender.send(new int[] {pair[0].getFd()});
                new java.io.DataOutputStream(clientSocket.getOutputStream()).writeInt(-1);
                bridge.awaitClosed();
                check(pair[0].getFileDescriptor().valid(), "invalid frame closed sender's descriptor");
            } finally {
                bridge.close(); pair[0].close(); pair[1].close();
            }
        }
        check(android.system.Os.openCount() == 0, "invalid frame leaked descriptors");
    }

    public static void main(String[] args) throws Exception {
        invalidWireDoesNotImport();
        failureKeepsReturnedDescriptor();
        invalidFrameClosesDescriptors();
        LinkedBlockingQueue<int[]> toClient = new LinkedBlockingQueue<>(), toServer = new LinkedBlockingQueue<>();
        try (ServerSocket listener = new ServerSocket(0);
             Socket clientSocket = new Socket("127.0.0.1", listener.getLocalPort());
             Socket serverSocket = listener.accept()) {
            Marker clientMarker = new Marker(clientSocket.getInputStream(), clientSocket.getOutputStream(), toClient, toServer);
            Marker serverMarker = new Marker(serverSocket.getInputStream(), serverSocket.getOutputStream(), toServer, toClient);
            java.util.concurrent.atomic.AtomicReference<ParcelFileDescriptor> serverReceived = new java.util.concurrent.atomic.AtomicReference<>();
            java.util.concurrent.CountDownLatch oneWay = new java.util.concurrent.CountDownLatch(1);
            IBinder root = new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    if (code == 2) {
                        ParcelFileDescriptor received = data.readTypedObject(ParcelFileDescriptor.CREATOR);
                        try { received.close(); }
                        catch (java.io.IOException e) { throw new IllegalStateException(e); }
                        oneWay.countDown();
                        return true;
                    }
                    if (code != 1 && code != 3) return false;
                    ParcelFileDescriptor received = data.readTypedObject(ParcelFileDescriptor.CREATOR);
                    serverReceived.set(received);
                    check(received.canDetectErrors(), "status descriptor is transferred");
                    if (code == 3)
                        received.writeToParcel(reply, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    else reply.writeTypedObject(received, android.os.Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    return true;
                }
            };
            BinderBridge server = new BinderBridge(serverSocket.getInputStream(), serverSocket.getOutputStream(),
                root, serverSocket::close, -1, 0, serverMarker);
            BinderBridge client = new BinderBridge(clientSocket.getInputStream(), clientSocket.getOutputStream(),
                null, clientSocket::close, -1, 0, clientMarker);
            ParcelFileDescriptor[] pair = ParcelFileDescriptor.createReliableSocketPair();
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeTypedObject(pair[0], 0);
                check(client.remoteRoot().transact(1, data, reply, 0), "round trip");
                ParcelFileDescriptor returned = reply.readTypedObject(ParcelFileDescriptor.CREATOR);
                check(returned.canDetectErrors(), "returned status descriptor");
                returned.closeWithError("failure");
                try { pair[1].checkError(); throw new AssertionError("missing remote error"); }
                catch (java.io.IOException expected) { check("failure".equals(expected.getMessage()), "error status"); }
                Parcel tooMany = Parcel.obtain(), ignored = Parcel.obtain();
                try {
                    for (int i = 0; i < 17; i++) tooMany.writeFileDescriptor(pair[1].getFileDescriptor());
                    try { client.remoteRoot().transact(1, tooMany, ignored, 0); throw new AssertionError("FD limit"); }
                    catch (android.os.RemoteException expected) {}
                    check(pair[1].getFileDescriptor().valid(), "failed transfer closed original");
                } finally { tooMany.recycle(); ignored.recycle(); }
                Parcel async = Parcel.obtain();
                try {
                    async.writeTypedObject(pair[1], 0);
                    check(client.remoteRoot().transact(2, async, null, IBinder.FLAG_ONEWAY), "one-way send");
                    check(oneWay.await(5, TimeUnit.SECONDS), "one-way delivery");
                } finally { async.recycle(); }
                ParcelFileDescriptor[] directPair = ParcelFileDescriptor.createReliableSocketPair();
                Parcel direct = Parcel.obtain(), directReply = Parcel.obtain();
                try {
                    direct.writeTypedObject(directPair[0], 0);
                    check(client.remoteRoot().transact(3, direct, directReply, 0), "direct return");
                    ParcelFileDescriptor directResult = ParcelFileDescriptor.CREATOR.createFromParcel(directReply);
                    check(directResult.canDetectErrors(), "direct return status descriptor");
                    directResult.close();
                } finally {
                    direct.recycle(); directReply.recycle(); directPair[0].close(); directPair[1].close();
                }
            } finally {
                data.recycle(); reply.recycle();
                if (serverReceived.get() != null) serverReceived.get().close();
                pair[0].close(); pair[1].close();
                client.close(); server.close();
                client.awaitClosed(); server.awaitClosed();
            }
        }
        check(android.system.Os.openCount() == 0, "descriptors leaked");
        System.out.println("BinderBridgeFdTest: PASS");
    }
}
