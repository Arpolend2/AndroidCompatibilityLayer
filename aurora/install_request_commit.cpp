#include "install_request_commit.h"
#include "app_storage_lock.h"

#include <QDir>
#include <QLockFile>
#include <QObject>
#include <QRegularExpression>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
bool identicalFiles(int first, int second, bool *identical)
{
    *identical = false;
    struct stat a, b;
    if (fstat(first, &a) != 0 || fstat(second, &b) != 0) return false;
    if (a.st_size != b.st_size) return true;
    char left[65536], right[65536];
    off_t offset = 0;
    while (offset < a.st_size) {
        const size_t size = size_t(qMin<off_t>(sizeof(left), a.st_size - offset));
        const ssize_t n = pread(first, left, size, offset);
        const ssize_t m = pread(second, right, size, offset);
        if (n <= 0 || m != n) return false;
        if (std::memcmp(left, right, size_t(n)) != 0) return true;
        offset += n;
    }
    *identical = true;
    return true;
}

void removeRpmCache(int appDir)
{
    const int cache = openat(appDir, "standalone-rpm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (cache < 0) return;
    const int mapping = openat(cache, ".last-created-app-id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    char value[128];
    const ssize_t length = mapping < 0 ? -1 : read(mapping, value, sizeof(value));
    if (mapping >= 0) close(mapping);
    const QByteArray id = length > 0 ? QByteArray(value, int(length)) : QByteArray();
    static const QRegularExpression pattern(
        QStringLiteral("\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}\\z"));
    if (pattern.match(QString::fromLatin1(id)).hasMatch()) {
        for (const char *arch : {"aarch64", "armv7hl", "x86_64"}) {
            const QByteArray name = id + "-1.0.0-1." + arch + ".rpm";
            if (unlinkat(cache, name.constData(), 0) != 0 && errno != ENOENT)
                qWarning("Could not remove the obsolete cached RPM.");
        }
    }
    if (unlinkat(cache, ".rpm-cache.json", 0) != 0 && errno != ENOENT)
        qWarning("Could not remove obsolete RPM cache metadata.");
    if (unlinkat(cache, ".last-created-app-id", 0) != 0 && errno != ENOENT)
        qWarning("Could not remove the obsolete standalone app mapping.");
    close(cache);
}
}

InstallRequestCommit::InstallRequestCommit(const QString &source, const QString &root, const QString &package,
                                           bool allowUpdate)
    : m_source(source), m_root(root), m_package(package), m_canceled(false), m_file(-1), m_rootFd(-1),
      m_allowUpdate(allowUpdate) {}

InstallRequestCommit::~InstallRequestCommit()
{
    cancel();
    wait();
    if (m_file >= 0) close(m_file);
    if (m_rootFd >= 0) close(m_rootFd);
}

void InstallRequestCommit::cancel()
{
    m_canceled.store(true);
}

void InstallRequestCommit::run()
{
    if (!QDir().mkpath(m_root)) {
        m_error = QObject::tr("Cannot create the app storage directory.");
        return;
    }
    const QByteArray root = m_root.toUtf8();
    m_rootFd = open(root.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (m_rootFd < 0) {
        m_error = QObject::tr("Cannot open the app storage directory safely.");
        return;
    }
#ifdef O_TMPFILE
    m_file = openat(m_rootFd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
    if (m_file < 0) {
        m_error = QObject::tr("This filesystem cannot stage an installation safely: %1.")
                      .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    const QByteArray path = m_source.toUtf8();
    const int input = open(path.constData(), O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        m_error = QObject::tr("Cannot open the approved APK snapshot.");
        return;
    }
    char buffer[65536];
    while (!m_canceled.load()) {
        const ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            m_error = QObject::tr("Cannot read the approved APK snapshot.");
            break;
        }
        if (!n) break;
        for (ssize_t offset = 0; offset < n;) {
            const ssize_t written = write(m_file, buffer + offset, static_cast<size_t>(n - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                m_error = QObject::tr("Cannot stage the approved APK.");
                break;
            }
            offset += written;
        }
        if (!m_error.isEmpty()) break;
    }
    close(input);
    if (m_canceled.load()) m_error = QObject::tr("Installation was canceled.");
    else if (m_error.isEmpty() && fsync(m_file) != 0) m_error = QObject::tr("Cannot save the approved APK.");
}

bool InstallRequestCommit::commit(QString *error)
{
    static const QRegularExpression packagePattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    if (isRunning() || m_canceled.load() || !m_error.isEmpty() || m_file < 0 || m_rootFd < 0 ||
        m_package.size() > 255 || !packagePattern.match(m_package).hasMatch()) {
        if (error) *error = m_error.isEmpty() ? QObject::tr("Installation is not ready.") : m_error;
        return false;
    }
    QLockFile lock(appStorageLockPath(m_root, m_package));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error) *error = QObject::tr("The app is running or its data is in use.");
        return false;
    }
    const QByteArray package = m_package.toUtf8();
    if (mkdirat(m_rootFd, package.constData(), 0700) != 0) {
        if (errno == EEXIST && m_allowUpdate) {
            const int dir = openat(m_rootFd, package.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat existing;
            if (dir < 0 || fstatat(dir, "base.apk", &existing, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(existing.st_mode)) {
                if (dir >= 0) close(dir);
                if (error) *error = QObject::tr("The installed APK cannot be updated safely.");
                return false;
            }
            const int oldApk = openat(dir, "base.apk", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            bool identical = false;
            const bool compared = oldApk >= 0 && identicalFiles(oldApk, m_file, &identical);
            if (oldApk >= 0) close(oldApk);
            if (!compared || m_canceled.load()) {
                close(dir);
                if (error) *error = QObject::tr("Cannot compare the installed APK safely.");
                return false;
            }
            if (identical) {
                close(dir);
                return true;
            }
            struct stat splits;
            if (fstatat(dir, "splits", &splits, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
                close(dir);
                if (error) *error = QObject::tr("A split APK installation cannot be replaced with a single APK.");
                return false;
            }
            const QByteArray name = (QStringLiteral(".update-") +
                                     QUuid::createUuid().toString()).toUtf8();
            const QByteArray staged = QByteArray("/proc/self/fd/") + QByteArray::number(m_file);
            if (m_canceled.load() || linkat(AT_FDCWD, staged.constData(), dir, name.constData(), AT_SYMLINK_FOLLOW) != 0) {
                close(dir);
                if (error) *error = QObject::tr("Cannot stage the APK update safely.");
                return false;
            }
            const int cache = openat(dir, "standalone-rpm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const QByteArray pending = name + "-cache";
            bool cacheMoved = false;
            if (cache < 0 && errno != ENOENT) {
                unlinkat(dir, name.constData(), 0);
                close(dir);
                if (error) *error = QObject::tr("Cannot invalidate the previous RPM cache safely.");
                return false;
            }
            if (cache >= 0) {
                if (renameat(cache, ".rpm-cache.json", cache, pending.constData()) == 0)
                    cacheMoved = true;
                else if (errno != ENOENT) {
                    unlinkat(dir, name.constData(), 0);
                    close(cache);
                    close(dir);
                    if (error) *error = QObject::tr("Cannot invalidate the previous RPM cache safely.");
                    return false;
                }
            }
#if defined(SYS_renameat2)
            const unsigned exchange = 2;
            if (m_canceled.load() || syscall(SYS_renameat2, dir, name.constData(), dir, "base.apk", exchange) != 0) {
                if (cacheMoved && renameat(cache, pending.constData(), cache, ".rpm-cache.json") != 0)
                    qWarning("Could not restore the RPM cache after a failed APK update.");
                if (cache >= 0) close(cache);
                unlinkat(dir, name.constData(), 0);
                close(dir);
                if (error) *error = QObject::tr("This filesystem cannot replace the APK safely.");
                return false;
            }
            if (fsync(dir) != 0) {
                const bool restored = syscall(SYS_renameat2, dir, name.constData(), dir, "base.apk", exchange) == 0;
                if (restored && cacheMoved && renameat(cache, pending.constData(), cache, ".rpm-cache.json") != 0)
                    qWarning("Could not restore the RPM cache after a failed APK update.");
                if (cache >= 0) close(cache);
                if (restored) unlinkat(dir, name.constData(), 0);
                if (!restored || fsync(dir) != 0)
                    qWarning("APK update rollback could not be fully confirmed; inspect the app library.");
                close(dir);
                if (error) *error = QObject::tr("The APK update could not be saved reliably; check the app library.");
                return false;
            }
            if (cacheMoved && unlinkat(cache, pending.constData(), 0) != 0)
                qWarning("Could not remove obsolete RPM cache metadata.");
            if (cache >= 0) close(cache);
            if (unlinkat(dir, name.constData(), 0) != 0 || fsync(dir) != 0) {
                removeRpmCache(dir);
                close(dir);
                if (error) *error = QObject::tr("The APK was updated, but its backup could not be removed reliably.");
                return false;
            }
            removeRpmCache(dir);
            close(dir);
            return true;
#else
            if (cacheMoved && renameat(cache, pending.constData(), cache, ".rpm-cache.json") != 0)
                qWarning("Could not restore the RPM cache after a failed APK update.");
            if (cache >= 0) close(cache);
            unlinkat(dir, name.constData(), 0);
            close(dir);
            if (error) *error = QObject::tr("This platform cannot replace the APK safely.");
            return false;
#endif
        }
        if (error) *error = errno == EEXIST ? QObject::tr("Updates are not supported.")
                                            : QObject::tr("Cannot create the app directory.");
        return false;
    }
    const int dir = openat(m_rootFd, package.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) {
        unlinkat(m_rootFd, package.constData(), AT_REMOVEDIR);
        if (error) *error = QObject::tr("Cannot open the app directory safely.");
        return false;
    }
    if (m_canceled.load()) {
        close(dir);
        unlinkat(m_rootFd, package.constData(), AT_REMOVEDIR);
        if (error) *error = QObject::tr("Installation was canceled.");
        return false;
    }
    const QByteArray staged = QByteArray("/proc/self/fd/") + QByteArray::number(m_file);
    if (linkat(AT_FDCWD, staged.constData(), dir, "base.apk", AT_SYMLINK_FOLLOW) != 0) {
        const QString cause = QString::fromLocal8Bit(std::strerror(errno));
        close(dir);
        unlinkat(m_rootFd, package.constData(), AT_REMOVEDIR);
        if (error) *error = QObject::tr("Cannot publish the approved APK: %1.").arg(cause);
        return false;
    }
    if (fsync(dir) != 0 || fsync(m_rootFd) != 0) {
        const bool removed = unlinkat(dir, "base.apk", 0) == 0;
        if (fsync(dir) != 0) qWarning("Could not synchronize installation rollback.");
        close(dir);
        const bool removedDir = unlinkat(m_rootFd, package.constData(), AT_REMOVEDIR) == 0;
        if (fsync(m_rootFd) != 0 || !removed || !removedDir)
            qWarning("Installation rollback could not be fully confirmed; inspect the app library.");
        if (error) *error = QObject::tr("Installation could not be saved reliably; check the app library.");
        return false;
    }
    close(dir);
    return true;
}
