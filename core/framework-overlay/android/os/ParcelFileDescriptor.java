// Overlay of core/framework/src/android/os/ParcelFileDescriptor.java (tools/build-framework.sh).
// It differs in what dup and the descriptors it is given own, which is now Android's:
//  - dup(FileDescriptor), dup() and fromFd(int) make a descriptor of their own (Os.dup), and the result owns it: close()
//    closes that copy, and the original stays as it was. The original's wrapped the same descriptor and owned nothing.
//  - adoptFd(int) takes the descriptor over: close() closes it.
//  - detachFd() gives the number away: close() no longer closes it.
//
// Why. Code duplicates a descriptor and then closes its own: GeckoView's GeckoRuntime.startCrashHelper (Firefox 152)
// makes two socket pairs with Os.socketpair, wraps each end with ParcelFileDescriptor.dup, closes the four originals,
// and hands the duplicates' numbers to Gecko with detachFd. With no duplicate those numbers were the closed ones' (-1, as
// libcore leaves a closed FileDescriptor), and Gecko's crash helper client stopped on "fd != -1" (a MOZ_CRASH) as Gecko
// started.
//
// And it goes into a Parcel and comes out of one as Android's does (writeToParcel, CREATOR): a duplicate of its descriptor,
// which the one read from the Parcel owns, so that the writer may close its own. It had neither, and the CREATOR was null:
// microG's People service hands Play Store the account's picture so (IPeopleCallbacks.onParcelFileDescriptor), and Play
// Store's stub stopped on a NullPointerException as it read it (no avatar).
//
// And the file it was opened from is said (meglerFile), for android.content.res.loader.ResourcesProvider.loadFromApk, which
// opens an APK by its descriptor: Google Play services' modules' resources.
//
// Everything else is the original's, unchanged.
package android.os;

import java.io.*;

public class ParcelFileDescriptor implements Parcelable, Closeable {
    public static final int MODE_READ_ONLY = 0x10000000, MODE_WRITE_ONLY = 0x20000000, MODE_READ_WRITE = 0x30000000, MODE_CREATE = 0x08000000,
        MODE_TRUNCATE = 0x04000000, MODE_APPEND = 0x02000000, MODE_WORLD_READABLE = 1, MODE_WORLD_WRITEABLE = 2;
    private final FileDescriptor mFd;
    private final Closeable mOwner;
    private final File mFile;
    private FileDescriptor mCommFd;
    private Integer mStatus;
    private String mStatusMessage;
    private boolean mClosed;
    private ParcelFileDescriptor mWrapped;
    ParcelFileDescriptor(FileDescriptor fd, Closeable owner, File f) { this(fd, owner, f, null); }
    private ParcelFileDescriptor(FileDescriptor fd, Closeable owner, File f, FileDescriptor comm) {
        mFd = fd; mOwner = owner; mFile = f; mCommFd = comm;
    }
    public ParcelFileDescriptor(ParcelFileDescriptor o) { this(o.mFd, o.mOwner, o.mFile, o.mCommFd); mWrapped = o; }
    public static int parseMode(String m) {
        int r;
        if ("r".equals(m)) r = MODE_READ_ONLY;
        else if ("w".equals(m) || "wt".equals(m)) r = MODE_WRITE_ONLY | MODE_CREATE | MODE_TRUNCATE;
        else if ("wa".equals(m)) r = MODE_WRITE_ONLY | MODE_CREATE | MODE_APPEND;
        else if ("rw".equals(m)) r = MODE_READ_WRITE | MODE_CREATE;
        else if ("rwt".equals(m)) r = MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE;
        else throw new IllegalArgumentException("Bad mode '" + m + "'");
        return r;
    }
    public static ParcelFileDescriptor open(File f, int mode) throws FileNotFoundException {
        try {
            if ((mode & MODE_READ_WRITE) == MODE_READ_ONLY) { FileInputStream in = new FileInputStream(f); return new ParcelFileDescriptor(in.getFD(), in, f); }
            if ((mode & MODE_READ_WRITE) == MODE_WRITE_ONLY) { FileOutputStream out = new FileOutputStream(f, (mode & MODE_APPEND) != 0); return new ParcelFileDescriptor(out.getFD(), out, f); }
            RandomAccessFile raf = new RandomAccessFile(f, "rw");
            if ((mode & MODE_TRUNCATE) != 0) raf.setLength(0);
            return new ParcelFileDescriptor(raf.getFD(), raf, f);
        } catch (FileNotFoundException e) { throw e; } catch (IOException e) { throw new FileNotFoundException(e.toString()); }
    }
    /** Megler: a FileDescriptor object for a descriptor number. */
    private static java.lang.reflect.Field meglerDescriptorField() throws ReflectiveOperationException {
        java.lang.reflect.Field field;
        try { field = FileDescriptor.class.getDeclaredField("descriptor"); }
        catch (NoSuchFieldException e) { field = FileDescriptor.class.getDeclaredField("fd"); }
        field.setAccessible(true);
        return field;
    }
    private static FileDescriptor meglerOf(int fd) {
        FileDescriptor f = new FileDescriptor();
        try { meglerDescriptorField().setInt(f, fd); }
        catch (ReflectiveOperationException e) { throw new IllegalStateException("File descriptor access is unavailable", e); }
        return f;
    }
    /** Megler: libcore's android.system.Os.dup or close, by reflection (the stubs the framework is compiled against have no Os). */
    private static Object meglerOs(String name, FileDescriptor fd) throws IOException {
        try {
            return Class.forName("android.system.Os").getMethod(name, FileDescriptor.class).invoke(null, fd);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new IOException(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IOException(e);
        }
    }
    /** Megler: what closes a descriptor this object owns, once. */
    private static Closeable meglerCloser(final FileDescriptor fd) {
        return () -> { if (fd.valid()) meglerOs("close", fd); };
    }
    /** Megler: a descriptor of its own for `fd` (Os.dup), which closing the result closes, as Android's dup makes. */
    private static ParcelFileDescriptor meglerDup(FileDescriptor fd, File f) throws IOException {
        FileDescriptor copy = (FileDescriptor) meglerOs("dup", fd);
        return new ParcelFileDescriptor(copy, meglerCloser(copy), f);
    }
    public static ParcelFileDescriptor dup(FileDescriptor fd) throws IOException { return meglerDup(fd, null); }
    public static ParcelFileDescriptor fromFd(int fd) throws IOException { return meglerDup(meglerOf(fd), null); }
    public static ParcelFileDescriptor adoptFd(int fd) { FileDescriptor f = meglerOf(fd); return new ParcelFileDescriptor(f, meglerCloser(f), null); }
    public static ParcelFileDescriptor[] createPipe() throws IOException {
        if (megler.SharedMicrogFdTransport.available()) {
            int[] fds = megler.SharedMicrogFdTransport.createPipe();
            return new ParcelFileDescriptor[] { adoptFd(fds[0]), adoptFd(fds[1]) };
        }
        try {
            FileDescriptor[] fds = (FileDescriptor[]) Class.forName("android.system.Os").getMethod("pipe").invoke(null);
            return new ParcelFileDescriptor[] {
                new ParcelFileDescriptor(fds[0], meglerCloser(fds[0]), null),
                new ParcelFileDescriptor(fds[1], meglerCloser(fds[1]), null)
            };
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new IOException(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IOException(e);
        }
    }
    public static ParcelFileDescriptor[] createReliablePipe() throws IOException { return withStatusChannel(createPipe()); }
    public ParcelFileDescriptor dup() throws IOException { return mWrapped != null ? mWrapped.dup() : meglerDup(mFd, mFile); }
    public FileDescriptor getFileDescriptor() { return mWrapped != null ? mWrapped.getFileDescriptor() : mFd; }
    public int getFd() {
        if (mWrapped != null) return mWrapped.getFd();
        try { return meglerDescriptorField().getInt(mFd); }
        catch (ReflectiveOperationException e) { throw new IllegalStateException("File descriptor access is unavailable", e); }
    }
    /* Detached: the number is the caller's, and closing this no longer closes it. */
    private boolean mDetached;
    public synchronized int detachFd() {
        if (mWrapped != null) return mWrapped.detachFd();
        if (mClosed || mDetached) throw new IllegalStateException("Already closed");
        int fd = getFd();
        try { sendStatus(2, null); }
        catch (IOException e) { System.err.println("ParcelFileDescriptor status delivery failed"); }
        mDetached = true;
        mClosed = true;
        return fd;
    }
    public long getStatSize() { return mFile != null ? mFile.length() : -1; }
    /** Megler: the file this was opened from (open), or null: what ResourcesProvider.loadFromApk opens the APK by. */
    public File meglerFile() { return mFile; }
    private static ParcelFileDescriptor[] withStatusChannel(ParcelFileDescriptor[] pair) throws IOException {
        ParcelFileDescriptor[] comm = null;
        try {
            comm = createSocketPair(5);
            for (ParcelFileDescriptor end : comm) {
                Class.forName("android.system.Os").getMethod("fcntlInt", FileDescriptor.class, int.class, int.class)
                    .invoke(null, end.mFd, 4, 04000);
            }
            return new ParcelFileDescriptor[] {
                new ParcelFileDescriptor(pair[0].mFd, pair[0].mOwner, pair[0].mFile, comm[0].mFd),
                new ParcelFileDescriptor(pair[1].mFd, pair[1].mOwner, pair[1].mFile, comm[1].mFd)
            };
        } catch (ReflectiveOperationException | IOException e) {
            for (ParcelFileDescriptor end : pair) try { end.close(); } catch (IOException closeError) { e.addSuppressed(closeError); }
            if (comm != null) for (ParcelFileDescriptor end : comm) try { end.close(); } catch (IOException closeError) { e.addSuppressed(closeError); }
            throw new IOException("Could not create descriptor status channel", e);
        }
    }
    private synchronized void readStatus() throws IOException {
        if (mCommFd == null || mStatus != null) return;
        byte[] bytes = new byte[1024];
        try {
            int n = (Integer) Class.forName("android.system.Os")
                .getMethod("read", FileDescriptor.class, byte[].class, int.class, int.class)
                .invoke(null, mCommFd, bytes, 0, bytes.length);
            if (n == 0) { mStatus = -2; return; }
            if (n < 4) throw new IOException("Invalid descriptor status");
            mStatus = ((bytes[0] & 255) << 24) | ((bytes[1] & 255) << 16) | ((bytes[2] & 255) << 8) | (bytes[3] & 255);
            if (mStatus == 1) mStatusMessage = new String(bytes, 4, n - 4, java.nio.charset.StandardCharsets.UTF_8);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable cause = e.getCause();
            try {
                if (cause.getClass().getField("errno").getInt(cause) == 11) return;
            } catch (ReflectiveOperationException ignored) {}
            throw new IOException("Could not read descriptor status", cause);
        } catch (ReflectiveOperationException e) {
            throw new IOException("Descriptor status channel is unavailable", e);
        }
    }
    private synchronized void sendStatus(int status, String message) throws IOException {
        if (mCommFd == null) return;
        FileDescriptor comm = mCommFd;
        IOException failure = null;
        try {
            if (status != -1) {
                readStatus();
                if (mStatus == null) {
                    byte[] text = message == null ? new byte[0] : message.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    byte[] bytes = new byte[4 + Math.min(text.length, 1020)];
                    bytes[0] = (byte)(status >>> 24); bytes[1] = (byte)(status >>> 16);
                    bytes[2] = (byte)(status >>> 8); bytes[3] = (byte)status;
                    System.arraycopy(text, 0, bytes, 4, bytes.length - 4);
                    int written = (Integer) Class.forName("android.system.Os")
                        .getMethod("write", FileDescriptor.class, byte[].class, int.class, int.class)
                        .invoke(null, comm, bytes, 0, bytes.length);
                    if (written != bytes.length) throw new IOException("Incomplete descriptor status");
                }
            }
        } catch (java.lang.reflect.InvocationTargetException e) {
            failure = new IOException("Could not send descriptor status", e.getCause());
        } catch (ReflectiveOperationException e) {
            failure = new IOException("Descriptor status channel is unavailable", e);
        } catch (IOException e) {
            failure = e;
        } finally {
            mCommFd = null;
            try { meglerOs("close", comm); }
            catch (IOException e) {
                if (failure == null) failure = e;
                else failure.addSuppressed(e);
            }
        }
        if (failure != null) throw failure;
    }
    private synchronized void closeWithStatus(int status, String message) throws IOException {
        if (mWrapped != null) { mWrapped.closeWithStatus(status, message); return; }
        if (mClosed) return;
        mClosed = true;
        IOException failure = null;
        try { sendStatus(status, message); } catch (IOException e) { failure = e; }
        try { if (mOwner != null && !mDetached) mOwner.close(); }
        catch (IOException e) {
            if (failure == null) failure = e;
            else failure.addSuppressed(e);
        }
        if (failure != null) throw failure;
    }
    public void close() throws IOException { closeWithStatus(0, null); }
    /** Complete a successful Binder return without sending a close status. */
    public void meglerCloseAfterTransfer() throws IOException { closeWithStatus(-1, null); }
    public void closeWithError(String msg) throws IOException {
        if (msg == null) throw new IllegalArgumentException("Error message is required");
        closeWithStatus(1, msg);
    }
    public synchronized void checkError() throws IOException {
        if (mWrapped != null) { mWrapped.checkError(); return; }
        readStatus();
        if (mStatus == null || mStatus == 0) return;
        if (mStatus == 2) throw new FileDescriptorDetachedException();
        if (mStatus == 1) throw new IOException(mStatusMessage);
        throw new IOException("Remote descriptor closed unexpectedly");
    }
    public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
    @Override
    public synchronized void writeToParcel(Parcel out, int flags) {
        if (mWrapped != null) { mWrapped.writeToParcel(out, flags); return; }
        try {
            out.writeInt(mCommFd == null ? 0 : 1);
            out.writeRawFileDescriptor(mFd);
            if (mCommFd != null) out.writeRawFileDescriptor(mCommFd);
            if ((flags & PARCELABLE_WRITE_RETURN_VALUE) != 0 && !out.meglerDeferFdReturn(this))
                closeWithStatus(-1, null);
        } catch (IOException e) {
            throw new IllegalStateException("Could not parcel file descriptor", e);
        }
    }
    public static final Parcelable.Creator<ParcelFileDescriptor> CREATOR = new Parcelable.Creator<ParcelFileDescriptor>() {
        @Override
        public ParcelFileDescriptor createFromParcel(Parcel in) {
            boolean hasComm = in.readInt() != 0;
            FileDescriptor fd = null, comm = null;
            try {
                fd = in.readRawFileDescriptor();
                comm = hasComm ? in.readRawFileDescriptor() : null;
                if (fd == null || (hasComm && comm == null))
                    throw new IllegalArgumentException("Invalid parcelled file descriptor");
                return new ParcelFileDescriptor(fd, meglerCloser(fd), null, comm);
            } catch (RuntimeException | Error failure) {
                if (fd != null) try { meglerOs("close", fd); } catch (IOException e) { failure.addSuppressed(e); }
                if (comm != null) try { meglerOs("close", comm); } catch (IOException e) { failure.addSuppressed(e); }
                throw failure;
            }
        }
        @Override
        public ParcelFileDescriptor[] newArray(int size) { return new ParcelFileDescriptor[size]; }
    };
    public static class AutoCloseInputStream extends FileInputStream {
        private final ParcelFileDescriptor mPfd;
        public AutoCloseInputStream(ParcelFileDescriptor p) { super(p.getFileDescriptor()); mPfd = p; }
        @Override public void close() throws IOException { try { super.close(); } finally { mPfd.close(); } }
        @Override public int read() throws IOException {
            int result = super.read();
            if (result < 0 && mPfd.canDetectErrors()) mPfd.checkError();
            return result;
        }
        @Override public int read(byte[] b, int off, int len) throws IOException {
            int result = super.read(b, off, len);
            if (result < 0 && mPfd.canDetectErrors()) mPfd.checkError();
            return result;
        }
        @Override public int read(byte[] b) throws IOException { return read(b, 0, b.length); }
    }
    public static class AutoCloseOutputStream extends FileOutputStream {
        private final ParcelFileDescriptor mPfd;
        public AutoCloseOutputStream(ParcelFileDescriptor p) { super(p.getFileDescriptor()); mPfd = p; }
        @Override public void close() throws IOException { try { super.close(); } finally { mPfd.close(); } }
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static ParcelFileDescriptor[] createReliableSocketPair() throws IOException { return createReliableSocketPair(1); }
    public static ParcelFileDescriptor[] createReliableSocketPair(int type) throws IOException {
        return withStatusChannel(createSocketPair(type));
    }
    public static ParcelFileDescriptor[] createSocketPair() throws IOException { return createSocketPair(1); }
    public static ParcelFileDescriptor[] createSocketPair(int type) throws IOException {
        if (megler.SharedMicrogFdTransport.available()) {
            int[] fds = megler.SharedMicrogFdTransport.createSocketPair(type);
            return new ParcelFileDescriptor[] { adoptFd(fds[0]), adoptFd(fds[1]) };
        }
        try {
            FileDescriptor first = new FileDescriptor(), second = new FileDescriptor();
            Class.forName("android.system.Os").getMethod("socketpair", int.class, int.class, int.class,
                FileDescriptor.class, FileDescriptor.class).invoke(null, 1, type, 0, first, second);
            return new ParcelFileDescriptor[] {
                new ParcelFileDescriptor(first, meglerCloser(first), null),
                new ParcelFileDescriptor(second, meglerCloser(second), null)
            };
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new IOException(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IOException(e);
        }
    }
    public static android.os.ParcelFileDescriptor fromData(byte[] p0, java.lang.String p1) { return null; }
    public static android.os.ParcelFileDescriptor fromDatagramSocket(java.net.DatagramSocket p0) { return null; }
    public static android.os.ParcelFileDescriptor fromFd(java.io.FileDescriptor p0, android.os.Handler p1, android.os.ParcelFileDescriptor.OnCloseListener p2) { return null; }
    public static android.os.ParcelFileDescriptor fromSocket(java.net.Socket p0) { return null; }
    public static java.io.File getFile(java.io.FileDescriptor p0) { return null; }
    public static android.os.ParcelFileDescriptor open(java.io.File p0, int p1, android.os.Handler p2, android.os.ParcelFileDescriptor.OnCloseListener p3) { return null; }
    public static android.os.ParcelFileDescriptor wrap(android.os.ParcelFileDescriptor p0, android.os.Handler p1, android.os.ParcelFileDescriptor.OnCloseListener p2) { return null; }
    public synchronized boolean canDetectErrors() { return mWrapped != null ? mWrapped.canDetectErrors() : mCommFd != null; }
    public void releaseResources() {}
    public long seekTo(long pos) throws IOException {
        if (mWrapped != null) return mWrapped.seekTo(pos);
        try {
            return (Long) Class.forName("android.system.Os")
                .getMethod("lseek", FileDescriptor.class, long.class, int.class).invoke(null, mFd, pos, 0);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new IOException("Could not seek descriptor", e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IOException("Descriptor seeking is unavailable", e);
        }
    }
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static class FileDescriptorDetachedException extends java.io.IOException {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public FileDescriptorDetachedException() { super(); }
    }
    public interface OnCloseListener {
        void onClose(java.io.IOException p0);
    }
    // ---- end of generated nested classes
}
