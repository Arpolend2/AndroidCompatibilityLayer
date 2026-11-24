import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import java.io.IOException;

public final class ParcelFileDescriptorReliableTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError();
    }
    public static void main(String[] args) throws Exception {
        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createReliablePipe();
        check(pipe[0].canDetectErrors() && pipe[1].canDetectErrors());
        pipe[0].checkError();
        pipe[1].closeWithError("test error");
        try { pipe[0].checkError(); throw new AssertionError(); }
        catch (IOException expected) { check("test error".equals(expected.getMessage())); }
        pipe[0].close();

        ParcelFileDescriptor[] sockets = ParcelFileDescriptor.createReliableSocketPair();
        Parcel parcel = Parcel.obtain();
        sockets[0].writeToParcel(parcel, 0);
        int beforeFailure = android.system.Os.openCount();
        android.system.Os.failDupAfter(1);
        parcel.setDataPosition(0);
        try { ParcelFileDescriptor.CREATOR.createFromParcel(parcel); throw new AssertionError(); }
        catch (IllegalStateException expected) { check(android.system.Os.openCount() == beforeFailure); }
        parcel.setDataPosition(0);
        ParcelFileDescriptor copied = ParcelFileDescriptor.CREATOR.createFromParcel(parcel);
        check(copied != null && copied.canDetectErrors());
        sockets[0].closeWithError("transferred");
        try { sockets[1].checkError(); throw new AssertionError(); }
        catch (IOException expected) { check("transferred".equals(expected.getMessage())); }
        copied.close(); sockets[1].close(); parcel.recycle();

        ParcelFileDescriptor[] detached = ParcelFileDescriptor.createReliableSocketPair();
        int raw = detached[0].detachFd();
        try { detached[1].checkError(); throw new AssertionError(); }
        catch (ParcelFileDescriptor.FileDescriptorDetachedException expected) {}
        android.system.Os.close(ParcelFileDescriptor.adoptFd(raw).getFileDescriptor());
        detached[1].close();
        check(android.system.Os.openCount() == 0);
        System.out.println("ParcelFileDescriptorReliableTest: PASS");
    }
}
