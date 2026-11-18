#include "log_channel.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
bool address(const QString &path, sockaddr_un *result)
{
    const QByteArray name = QFile::encodeName(path);
    if (name.isEmpty() || size_t(name.size()) >= sizeof(result->sun_path)) return false;
    std::memset(result, 0, sizeof(*result));
    result->sun_family = AF_UNIX;
    std::memcpy(result->sun_path, name.constData(), size_t(name.size()) + 1);
    return true;
}
}

LogChannel::LogChannel(QObject *parent) : QObject(parent)
{
    if (!m_directory.isValid()) {
        qWarning() << "Core log display unavailable: temporary directory creation failed";
        return;
    }
    m_path = QDir(m_directory.path()).filePath(QStringLiteral("log"));
    sockaddr_un target;
    if (!address(m_path, &target)) {
        qWarning() << "Core log display unavailable: socket path is too long";
        m_path.clear();
        return;
    }
    m_socket = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_socket < 0 || ::bind(m_socket, reinterpret_cast<sockaddr *>(&target), sizeof(target)) < 0) {
        qWarning() << "Core log display unavailable: socket setup failed:" << std::strerror(errno);
        if (m_socket >= 0) ::close(m_socket);
        m_socket = -1;
        m_path.clear();
        return;
    }
    m_notifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this](int) { readMessages(); });
    m_update = new QTimer(this);
    m_update->setSingleShot(true);
    m_update->setInterval(100);
    connect(m_update, &QTimer::timeout, this, &LogChannel::changed);
}

LogChannel::~LogChannel()
{
    if (m_notifier) m_notifier->setEnabled(false);
    if (m_socket >= 0) ::close(m_socket);
    if (!m_path.isEmpty()) QFile::remove(m_path);
}

int LogChannel::openSender(const QString &path)
{
    sockaddr_un target;
    if (!address(path, &target)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void MEGLER_CB LogChannel::sendLine(int, const char *line, void *user)
{
    const int fd = *static_cast<const int *>(user);
    if (line && fd >= 0) ::send(fd, line, std::strlen(line), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void LogChannel::readMessages()
{
    char bytes[4096];
    bool changed = false;
    for (int count = 0; count < 256; ++count) {
        const ssize_t size = ::recv(m_socket, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                qWarning() << "Core log display read failed:" << std::strerror(errno);
            break;
        }
        m_text += QString::fromUtf8(bytes, int(size));
        m_text += QLatin1Char('\n');
        if (m_text.size() > 131072) {
            const int cut = m_text.indexOf(QLatin1Char('\n'), m_text.size() - 131072);
            m_text.remove(0, cut < 0 ? m_text.size() - 131072 : cut + 1);
        }
        changed = true;
    }
    if (changed && !m_update->isActive()) m_update->start();
}
