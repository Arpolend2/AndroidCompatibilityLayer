#include "single_instance_lock.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

SingleInstanceLock::SingleInstanceLock(QObject *parent, int retryMilliseconds, int maxRetries)
    : QObject(parent), m_maxRetries(maxRetries)
{
    m_retry.setInterval(retryMilliseconds);
    connect(&m_retry, &QTimer::timeout, this, [this]() {
        if (!m_wanted) {
            m_retry.stop();
            return;
        }
        if (tryAcquire()) {
            m_retry.stop();
            qInfo().noquote() << "Single-instance lock reacquired after" << m_retries + 1 << "retries";
            return;
        }
        if (++m_retries >= m_maxRetries) {
            m_retry.stop();
            qWarning().noquote() << "Single-instance lock not reacquired after" << m_retries
                                 << "retries; launches will start a forwarding process:" << m_path;
        }
    });
}

SingleInstanceLock::~SingleInstanceLock()
{
    release();
}

bool SingleInstanceLock::adoptInherited()
{
    if (m_fd >= 0) return true;
    const QDir fds(QStringLiteral("/proc/self/fd"));
    QString rejected;
    for (const QString &name : fds.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const int fd = name.toInt(&ok);
        if (!ok || fd <= STDERR_FILENO) continue;
        const QString target = QFile::symLinkTarget(fds.filePath(name));
        if (!target.contains(QLatin1String("/single-instance-locks/")) ||
            !target.endsWith(QLatin1String("/instance.lock"))) continue;
        struct flock probe = {};
        probe.l_type = F_WRLCK;
        probe.l_whence = SEEK_SET;
        probe.l_len = 1;
        // F_GETLK ignores our own locks, so a held lock reports F_UNLCK here.
        if (fcntl(fd, F_GETLK, &probe) != 0 || probe.l_type != F_UNLCK) {
            rejected = target;
            continue;
        }
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        m_fd = fd;
        m_path = QDir::cleanPath(target);
        m_wanted = true;
        return true;
    }
    if (!rejected.isEmpty())
        qWarning().noquote() << "Single-instance lock descriptor not owned by this process:" << rejected;
    else
        qInfo() << "No inherited single-instance lock (not started by the launcher with --single-instance)";
    return false;
}

void SingleInstanceLock::setHeld(bool held)
{
    if (held && m_wanted && m_retry.isActive()) return;
    m_wanted = held;
    m_retries = 0;
    if (!held) {
        m_retry.stop();
        release();
        return;
    }
    if (m_path.isEmpty() || tryAcquire()) {
        m_retry.stop();
        return;
    }
    qInfo().noquote() << "Single-instance lock busy, retrying:" << m_path << qt_error_string(m_lastErrno);
    m_retry.start();
}

bool SingleInstanceLock::tryAcquire()
{
    if (m_fd >= 0) return true;
    const int fd = ::open(QFile::encodeName(m_path).constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        m_lastErrno = errno;
        return false;
    }
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 1;
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        m_lastErrno = errno;
        ::close(fd);
        return false;
    }
    m_fd = fd;
    return true;
}

void SingleInstanceLock::release()
{
    if (m_fd < 0) return;
    // Closing any descriptor of the file drops this process's fcntl locks on it.
    ::close(m_fd);
    m_fd = -1;
}
