/*
 * Megler's table of the file locks a process holds, in place of Android 9's (the same DEX descriptors, sun.nio.ch.FileLockTable
 * and SharedFileLockTable, with every member FileChannelImpl and the asynchronous file channels use of them; this DEX comes
 * before core-oj's).
 *
 * A FileChannel's lock is the process's own, held against the other processes: while one holds it, another's tryLock() answers
 * null and its lock() waits. Within one process the library keeps a table of the locks taken through its channels, and one
 * that overlaps another there is the program's error: OverlappingFileLockException, which is not an IOException and which
 * nobody expects where a null is. That table is Android 9's SharedFileLockTable, one for the whole virtual machine.
 *
 * Megler runs in one virtual machine what Android runs in several processes: an app's other processes, and the Google Play
 * services an app talks to, are class scopes of their own here (megler.Processes, megler.ServiceClassLoader). Apps guard the
 * files their processes share with such locks, and with one table for all of them the second process got the error in
 * place of the answer:
 *
 *   - the Play Store (54.1, a Lumia 950, 0.1.0.699): "E/Finsky:background: VS: Failed to update class ...
 *     java.nio.channels.OverlappingFileLockException at sun.nio.ch.FileChannelImpl.tryLock" in its value store, which its main
 *     process and its :background process both write;
 *   - Google Play services (26.41, 0.1.0.690): its Chimera configuration's .config.lock, taken with lock() by its own process
 *     and by its .unstable one -- "E/DGFRR: FR error" and an exception nothing caught on GoogleApiHandler.
 *
 * So the table knows whose each lock is: the class scope of the code that takes it, which the runtime tells (scope(), in
 * dvm/mg_dvm_art.c). Between locks of one scope the rule is Android's for one process: an overlapping one is the error.
 * Between scopes it is the system's for two processes: tryLock() answers null, lock() waits until the other is released or its
 * channel closed, and two shared locks may overlap. The system itself keeps no locks here (the core's fcntl grants every one:
 * bionic/mg_libc_io.c), so this table is where they are.
 *
 * The table's add() cannot refuse or wait: FileChannelImpl calls it before it asks the system for the lock, and takes the
 * answer from FileDispatcherImpl.lock(). So add() notes the lock as wanted, for the thread that asks, and
 * FileDispatcherImpl.lock() (FileDispatcherImpl.java.replace) calls contend() on that thread before its native call: it grants
 * the lock, or answers what the system would. An asynchronous channel asks the system from another thread, which contend()
 * cannot tell from any other: its locks are granted at once.
 *
 * A lock lasts as the system's does: until it is released, or its channel is closed. The lock's object may be forgotten
 * before that (AndroidX's ProcessLock keeps the channel, not the FileLock): then it stays, in the other scopes' way, while
 * its channel is open. A channel nobody closed, collected, holds nothing.
 *
 * Not here: a lock between two apps' processes, which are processes of Windows (each has a table of its own, and the core
 * grants both); the system's detection of a deadlock; and the order of those who wait, which is none.
 */
package sun.nio.ch;

import java.io.FileDescriptor;
import java.io.IOException;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import java.nio.channels.Channel;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;

abstract class FileLockTable {
    protected FileLockTable() {
    }

    /**
     * Creates and returns a file lock table for a channel that is connected to
     * the a system-wide map of all file locks for the Java virtual machine.
     */
    public static FileLockTable newSharedFileLockTable(Channel channel,
                                                       FileDescriptor fd)
        throws IOException
    {
        return new SharedFileLockTable(channel, fd);
    }

    /**
     * Adds a file lock to the table.
     *
     * @throws OverlappingFileLockException if the file lock overlaps
     *         with an existing file lock in the table
     */
    public abstract void add(FileLock fl) throws OverlappingFileLockException;

    /**
     * Remove an existing file lock from the table.
     */
    public abstract void remove(FileLock fl);

    /**
     * Removes all file locks from the table.
     *
     * @return  The list of file locks removed
     */
    public abstract List<FileLock> removeAll();

    /**
     * Replaces an existing file lock in the table.
     */
    public abstract void replace(FileLock fl1, FileLock fl2);
}


/**
 * A file lock table that is over a system-wide map of all file locks.
 */
class SharedFileLockTable extends FileLockTable {

    /** The channel a table's locks are taken through, held weakly: they last while it is open. */
    private static final class Holder extends WeakReference<Channel> {
        final FileKey key;

        Holder(Channel channel, FileKey key) {
            super(channel, queue);
            this.key = key;
        }

        boolean open() {
            Channel c = get();
            return c != null && c.isOpen();
        }
    }

    /**
     * A lock of a file: its FileLock, held weakly, with what the table needs of it once it is collected -- whose it is, where
     * in the file, and whether it was granted (`held`, read and written under the list's monitor).
     */
    private static final class Entry extends WeakReference<FileLock> {
        final Holder by;
        final List<Entry> list;
        final int scope;
        final long position, size;
        final boolean shared;
        boolean held;

        Entry(FileLock fl, Holder by, List<Entry> list, int scope, boolean held) {
            super(fl, queue);
            this.by = by;
            this.list = list;
            this.scope = scope;
            this.position = fl.position();
            this.size = fl.size();
            this.shared = fl.isShared();
            this.held = held;
        }

        boolean overlaps(Entry other) {
            return position + size > other.position && other.position + other.size > position;
        }

        /** Nothing holds it any more: its FileLock was collected, and it was never granted or its channel is closed or collected. */
        boolean dead() {
            return get() == null && !(held && by.open());
        }
    }

    // The system-wide map is keyed on the file's identity; the value is the list of the locks of that file, every access
    // to which is under the list's own monitor. A list that became empty leaves the map under that monitor too.
    private static final ConcurrentHashMap<FileKey, List<Entry>> lockMap = new ConcurrentHashMap<FileKey, List<Entry>>();

    // The locks and the channels the collector cleared
    private static final ReferenceQueue<Object> queue = new ReferenceQueue<Object>();

    // The lock this thread added and was not granted yet: what its FileDispatcherImpl.lock() is about (contend())
    private static final ThreadLocal<Entry> wanted = new ThreadLocal<Entry>();

    // File key for the file that this channel is connected to
    private final FileKey fileKey;

    private final Holder by;

    // Whether the channel asks the system for its locks on the thread that added them (a FileChannel does)
    private final boolean contends;

    SharedFileLockTable(Channel channel, FileDescriptor fd) throws IOException {
        this.fileKey = FileKey.create(fd);
        this.by = new Holder(channel, fileKey);
        this.contends = channel instanceof FileChannel;
    }

    /*
     * The process, as Android has it, of the code that asks: 0 for the app's own, another number for each class scope that
     * is a process of its own here. A runtime without the answer has one process, as Android 9's table had.
     */
    private static native int scope();

    private static boolean oneProcess;

    private static int asker() {
        if (oneProcess) return 0;
        try {
            return scope();
        } catch (LinkageError e) {
            oneProcess = true;
            return 0;
        }
    }

    @Override
    public void add(FileLock fl) throws OverlappingFileLockException {
        int scope = asker();
        wanted.remove();
        for (;;) {
            List<Entry> list = lockMap.get(fileKey);
            if (list == null) {
                List<Entry> made = new ArrayList<Entry>(2), there = lockMap.putIfAbsent(fileKey, made);
                list = there != null ? there : made;
            }
            synchronized (list) {
                // (Emptied and taken out of the map meanwhile: again, with the map's.)
                if (lockMap.get(fileKey) != list) continue;
                Entry e = new Entry(fl, by, list, scope, false);
                for (Iterator<Entry> i = list.iterator(); i.hasNext(); ) {
                    Entry x = i.next();
                    if (x.dead()) {
                        i.remove();
                        continue;
                    }
                    if (x.scope != scope || !x.overlaps(e)) continue;
                    // This process's own: the error, as on Android -- unless its FileLock is forgotten, which Android's table
                    // forgets with it; the new lock is in its place then, as the system has the process's newer lock.
                    if (x.get() != null) {
                        e.clear();
                        throw new OverlappingFileLockException();
                    }
                    i.remove();
                }
                e.held = !contends || !refused(e);
                list.add(e);
                if (!e.held) wanted.set(e);
                break;
            }
        }
        forgetCollected();
    }

    /** Whether another process holds a lock that `e` cannot be granted beside. Under the list's monitor. */
    private static boolean refused(Entry e) {
        for (Entry x : e.list) {
            if (x == e || x.scope == e.scope || !x.held || !x.overlaps(e) || (x.shared && e.shared)) continue;
            if (x.by.open()) return true;
        }
        return false;
    }

    /**
     * The system's answer to the lock this thread just added, asked by FileDispatcherImpl.lock() before its native call:
     * FileDispatcher.LOCKED when nothing here is against it (granted, or not one of this table's to decide: the native call
     * follows), NO_LOCK when another process holds it and the thread does not wait, INTERRUPTED when its wait was
     * interrupted (FileChannelImpl asks again while its channel is open).
     */
    static int contend(boolean blocking) {
        Entry e = wanted.get();
        if (e == null) return FileDispatcher.LOCKED;
        // (An interrupt closes the channel -- AbstractInterruptibleChannel -- a moment after it sets the thread's flag:
        // with the flag set at the start the wait is a short one, so that FileChannelImpl's asking again does not spin.)
        boolean interrupted = Thread.interrupted();
        try {
            synchronized (e.list) {
                for (;;) {
                    if (!e.list.contains(e)) {
                        // The channel was closed under the one who waits: its close is told by FileChannelImpl.
                        wanted.remove();
                        return blocking && !e.by.open() ? FileDispatcher.INTERRUPTED : FileDispatcher.LOCKED;
                    }
                    if (!refused(e)) {
                        e.held = true;
                        wanted.remove();
                        return FileDispatcher.LOCKED;
                    }
                    // (FileChannelImpl takes the lock out of the table then: remove(), which forgets it here.)
                    if (!blocking) return FileDispatcher.NO_LOCK;
                    try {
                        // (Not for good: a holder's channel collected, or closed by its file's finalizer, tells nobody.)
                        e.list.wait(interrupted ? 10 : 250);
                    } catch (InterruptedException ex) {
                        interrupted = true;
                    }
                    if (interrupted) return FileDispatcher.INTERRUPTED;
                }
            }
        } finally {
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    @Override
    public void remove(FileLock fl) {
        assert fl != null;

        // the lock must exist so the list of locks must be present
        List<Entry> list = lockMap.get(fileKey);
        if (list == null) return;

        synchronized (list) {
            for (int index = 0; index < list.size(); index++) {
                Entry e = list.get(index);
                if (e.get() != fl) continue;
                e.clear();
                list.remove(index);
                if (wanted.get() == e) wanted.remove();
                break;
            }
            released(fileKey, list);
        }
    }

    @Override
    public List<FileLock> removeAll() {
        List<FileLock> result = new ArrayList<FileLock>();
        List<Entry> list = lockMap.get(fileKey);
        if (list != null) {
            synchronized (list) {
                for (Iterator<Entry> i = list.iterator(); i.hasNext(); ) {
                    Entry e = i.next();
                    // the locks of this table's channel: the forgotten ones too, which lasted while it was open
                    if (e.by != by) continue;
                    FileLock lock = e.get();
                    if (lock != null) result.add(lock);
                    e.clear();
                    i.remove();
                }
                released(fileKey, list);
            }
        }
        return result;
    }

    @Override
    public void replace(FileLock fromLock, FileLock toLock) {
        // the lock must exist so there must be a list
        List<Entry> list = lockMap.get(fileKey);
        assert list != null;
        if (list == null) return;

        synchronized (list) {
            for (int index = 0; index < list.size(); index++) {
                Entry e = list.get(index);
                if (e.get() != fromLock) continue;
                e.clear();
                list.set(index, new Entry(toLock, by, list, e.scope, e.held));
                if (wanted.get() == e) wanted.remove();
                break;
            }
        }
    }

    /** A list some locks left: those who wait for them look again, and an empty list leaves the map. Under its monitor. */
    private static void released(FileKey key, List<Entry> list) {
        if (list.isEmpty()) lockMap.remove(key, list);
        list.notifyAll();
    }

    /** The locks and channels the collector cleared take what is dead out of their files' lists. */
    private static void forgetCollected() {
        Reference<?> ref;
        while ((ref = queue.poll()) != null) {
            FileKey key = ref instanceof Holder ? ((Holder) ref).key : ((Entry) ref).by.key;
            List<Entry> list = lockMap.get(key);
            if (list == null) continue;
            synchronized (list) {
                for (Iterator<Entry> i = list.iterator(); i.hasNext(); ) if (i.next().dead()) i.remove();
                released(key, list);
            }
        }
    }
}
