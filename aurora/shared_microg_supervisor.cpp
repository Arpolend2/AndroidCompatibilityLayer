#include "shared_microg_supervisor.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QVector>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
const int HeaderSize = 28;
const int MaxPackageBytes = 255;
const int StartupTimeoutMs = 15000;
const int RequestTimeoutMs = 5000;

void write32(char *buffer, quint32 value)
{
    buffer[0] = static_cast<char>(value >> 24);
    buffer[1] = static_cast<char>(value >> 16);
    buffer[2] = static_cast<char>(value >> 8);
    buffer[3] = static_cast<char>(value);
}

void write64(char *buffer, quint64 value)
{
    for (int i = 7; i >= 0; --i) {
        buffer[i] = static_cast<char>(value);
        value >>= 8;
    }
}

quint32 read32(const char *buffer)
{
    return (static_cast<quint32>(static_cast<unsigned char>(buffer[0])) << 24) |
           (static_cast<quint32>(static_cast<unsigned char>(buffer[1])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(buffer[2])) << 8) |
           static_cast<quint32>(static_cast<unsigned char>(buffer[3]));
}

quint64 read64(const char *buffer)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(buffer[i]);
    return value;
}

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}
}

SharedMicrogSupervisor::SharedMicrogSupervisor(QObject *parent)
    : QObject(parent)
{
    m_reaper.setInterval(250);
    connect(&m_reaper, &QTimer::timeout, this, &SharedMicrogSupervisor::reapChild);
}

SharedMicrogSupervisor::~SharedMicrogSupervisor()
{
    stop();
    delete m_socketNotifier;
    m_socketNotifier = nullptr;
}

bool SharedMicrogSupervisor::configure(const QString &program, const QStringList &arguments, QString *error)
{
    if (error) error->clear();
    if (m_pid > 0 || !m_channels.isEmpty()) {
        setError(error, QStringLiteral("Cannot replace a running shared microG worker."));
        return false;
    }
    const QFileInfo executable(program);
    if (!executable.isAbsolute() || !executable.isFile() || !executable.isExecutable() ||
        executable.isSymLink()) {
        setError(error, QStringLiteral("The shared microG worker executable is unavailable or untrusted."));
        return false;
    }
    m_program = executable.absoluteFilePath();
    m_arguments = arguments;
    return true;
}

bool SharedMicrogSupervisor::start(QString *error)
{
    if (m_pid > 0 && m_ready) return true;
    if (!isConfigured()) {
        setError(error, QStringLiteral("The shared microG worker is not configured."));
        return false;
    }

    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        setError(error, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const int parentFd = ::fcntl(sockets[0], F_DUPFD_CLOEXEC, 4);
    const int childFd = ::fcntl(sockets[1], F_DUPFD_CLOEXEC, 4);
    const int duplicateError = errno;
    ::close(sockets[0]);
    ::close(sockets[1]);
    if (parentFd < 0 || childFd < 0) {
        if (parentFd >= 0) ::close(parentFd);
        if (childFd >= 0) ::close(childFd);
        setError(error, QString::fromLocal8Bit(std::strerror(duplicateError)));
        return false;
    }

    posix_spawn_file_actions_t actions;
    int status = ::posix_spawn_file_actions_init(&actions);
    const bool actionsInitialized = status == 0;
    if (status == 0) status = ::posix_spawn_file_actions_adddup2(&actions, childFd, 3);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, parentFd);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, childFd);

    QList<QByteArray> encodedArguments;
    encodedArguments << m_program.toLocal8Bit()
                     << QByteArrayLiteral("--shared-microg-worker")
                     << QByteArrayLiteral("--headless")
                     << QByteArrayLiteral("--control-fd=3")
                     << QByteArrayLiteral("--protocol-version=1");
    for (const QString &argument : m_arguments)
        encodedArguments << argument.toLocal8Bit();
    QVector<char *> argv;
    argv.reserve(encodedArguments.size() + 1);
    for (QByteArray &argument : encodedArguments)
        argv.append(argument.data());
    argv.append(nullptr);

    if (status == 0)
        status = ::posix_spawn(&m_pid, encodedArguments.first().constData(), &actions, nullptr,
                               argv.data(), environ);
    if (actionsInitialized) ::posix_spawn_file_actions_destroy(&actions);
    ::close(childFd);
    if (status != 0) {
        ::close(parentFd);
        m_pid = -1;
        setError(error, QString::fromLocal8Bit(std::strerror(status)));
        return false;
    }

    m_controlFd = parentFd;
    const int flags = ::fcntl(m_controlFd, F_GETFL);
    if (flags < 0 || ::fcntl(m_controlFd, F_SETFL, flags | O_NONBLOCK) != 0) {
        setError(error, QStringLiteral("Cannot configure the shared microG control socket."));
        failWorker();
        return false;
    }
    if (!waitForReply(Ready, 0, error)) {
        failWorker();
        return false;
    }
    m_ready = true;
    if (m_socketNotifier) delete m_socketNotifier;
    m_socketNotifier = new QSocketNotifier(m_controlFd, QSocketNotifier::Read, this);
    connect(m_socketNotifier, &QSocketNotifier::activated, this,
            [this](int) { readUnsolicited(); });
    m_reaper.start();
    return true;
}

bool SharedMicrogSupervisor::sendMessage(MessageType type, quint64 channelId, quint32 uid, quint32 pid,
                                     const QByteArray &package, int fd, QString *error)
{
    const bool open = type == Open;
    const bool close = type == Close && channelId != 0;
    const bool stop = type == Stop && channelId == 0;
    if (m_controlFd < 0 || (!open && !close && !stop) || package.size() > MaxPackageBytes ||
        (open ? (channelId == 0 || fd < 0 || package.isEmpty() || uid == 0 || pid == 0)
              : (fd >= 0 || !package.isEmpty() || uid != 0 || pid != 0))) {
        setError(error, QStringLiteral("Invalid shared microG control message."));
        return false;
    }
    QByteArray payload(HeaderSize + package.size(), '\0');
    std::memcpy(payload.data(), "MGR1", 4);
    write32(payload.data() + 4, static_cast<quint32>(type));
    write64(payload.data() + 8, channelId);
    write32(payload.data() + 16, uid);
    write32(payload.data() + 20, pid);
    write32(payload.data() + 24, static_cast<quint32>(package.size()));
    if (!package.isEmpty())
        std::memcpy(payload.data() + HeaderSize, package.constData(), static_cast<size_t>(package.size()));

    struct iovec vector;
    vector.iov_base = payload.data();
    vector.iov_len = static_cast<size_t>(payload.size());
    struct msghdr message = {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))];
    if (type == Open) {
        std::memset(control, 0, sizeof(control));
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }
    const ssize_t sent = ::sendmsg(m_controlFd, &message, MSG_NOSIGNAL);
    if (sent != payload.size()) {
        setError(error, QStringLiteral("Cannot send shared microG control message: %1")
                 .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    return true;
}

bool SharedMicrogSupervisor::receiveMessage(Message *message, QString *error, bool *wouldBlock)
{
    if (wouldBlock) *wouldBlock = false;
    char payload[HeaderSize + MaxPackageBytes];
    char control[CMSG_SPACE(sizeof(int) * 16)];
    struct iovec vector;
    vector.iov_base = payload;
    vector.iov_len = sizeof(payload);
    struct msghdr received = {};
    received.msg_iov = &vector;
    received.msg_iovlen = 1;
    received.msg_control = control;
    received.msg_controllen = sizeof(control);
    const ssize_t count = ::recvmsg(m_controlFd, &received, MSG_CMSG_CLOEXEC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (wouldBlock) *wouldBlock = true;
        return false;
    }
    bool unexpectedFd = false;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&received); header;
         header = CMSG_NXTHDR(&received, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            unexpectedFd = true;
            continue;
        }
        const size_t bytes = header->cmsg_len - CMSG_LEN(0);
        const size_t descriptors = bytes / sizeof(int);
        const int *fds = reinterpret_cast<const int *>(CMSG_DATA(header));
        for (size_t i = 0; i < descriptors; ++i)
            ::close(fds[i]);
        if (descriptors != 0) unexpectedFd = true;
    }
    if (count < 0 || count == 0 || (received.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        unexpectedFd || count != HeaderSize || std::memcmp(payload, "MGR1", 4) != 0) {
        setError(error, QStringLiteral("Invalid shared microG worker response."));
        return false;
    }
    message->type = static_cast<MessageType>(read32(payload + 4));
    message->channelId = read64(payload + 8);
    message->uid = read32(payload + 16);
    message->pid = read32(payload + 20);
    message->packageLength = read32(payload + 24);
    if (message->uid != 0 || message->pid != 0 || message->packageLength != 0) {
        setError(error, QStringLiteral("Unexpected fields in shared microG worker response."));
        return false;
    }
    return true;
}

bool SharedMicrogSupervisor::waitForReply(MessageType expected, quint64 channelId, QString *error,
                                          bool *rejected, bool *disconnected)
{
    if (rejected) *rejected = false;
    if (disconnected) *disconnected = false;
    QElapsedTimer elapsed;
    elapsed.start();
    const int timeout = expected == Ready ? StartupTimeoutMs : RequestTimeoutMs;
    while (elapsed.elapsed() < timeout) {
        if (!checkChild(true)) {
            setError(error, QStringLiteral("The shared microG worker exited."));
            return false;
        }
        struct pollfd descriptor;
        descriptor.fd = m_controlFd;
        descriptor.events = POLLIN | POLLHUP | POLLERR;
        descriptor.revents = 0;
        const int remaining = timeout - static_cast<int>(elapsed.elapsed());
        const int ready = ::poll(&descriptor, 1, qMin(100, remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) {
            setError(error, QStringLiteral("Cannot wait for the shared microG worker: %1")
                     .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return false;
        }
        if (ready == 0) continue;
        Message reply;
        if (!receiveMessage(&reply, error)) return false;
        if (reply.type == Disconnected) {
            if (reply.channelId == 0) {
                setError(error, QStringLiteral("Invalid shared microG DISCONNECTED message."));
                return false;
            }
            handleDisconnected(reply.channelId);
            if (expected != Ready && reply.channelId == channelId) {
                if (m_closingChannels.contains(channelId)) continue;
                if (disconnected) *disconnected = true;
                setError(error, QStringLiteral("The shared microG bridge disconnected."));
                return false;
            }
            continue;
        }
        const bool validReady = expected == Ready && reply.type == Ready && reply.channelId == 0;
        const bool validAck = expected != Ready && reply.type == Ack && reply.channelId == channelId;
        const bool validNack = expected != Ready && reply.type == Nack && reply.channelId == channelId;
        if (validNack) {
            if (rejected) *rejected = true;
            setError(error, QStringLiteral("The shared microG worker rejected the channel."));
            return false;
        }
        if (!validReady && !validAck) {
            setError(error, QStringLiteral("Unexpected shared microG worker response."));
            return false;
        }
        return true;
    }
    setError(error, QStringLiteral("Timed out waiting for the shared microG worker."));
    return false;
}

bool SharedMicrogSupervisor::openChannel(const QString &package, int uid, int pid, quint64 channelId,
                                    int serverFd, QString *error)
{
    if (error) error->clear();
    static const QRegularExpression packagePattern(QStringLiteral(
        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    const QByteArray packageBytes = package.toUtf8();
    if (!channelId || uid <= 0 || pid <= 0 || packageBytes.size() > MaxPackageBytes ||
        !packagePattern.match(package).hasMatch() || m_usedChannelIds.contains(channelId)) {
        setError(error, QStringLiteral("Invalid shared microG channel identity."));
        return false;
    }
    if (!start(error)) return false;
    m_usedChannelIds.insert(channelId);
    m_disconnectedChannels.remove(channelId);
    m_openingChannels.insert(channelId);
    if (!sendMessage(Open, channelId, static_cast<quint32>(uid), static_cast<quint32>(pid),
                     packageBytes, serverFd, error)) {
        m_openingChannels.remove(channelId);
        failWorker();
        return false;
    }
    bool rejected = false;
    bool disconnected = false;
    if (!waitForReply(Ack, channelId, error, &rejected, &disconnected)) {
        m_openingChannels.remove(channelId);
        if (rejected || disconnected) {
            if (m_channels.isEmpty() && m_openingChannels.isEmpty()) stop();
            return false;
        }
        failWorker();
        return false;
    }
    m_openingChannels.remove(channelId);
    if (m_disconnectedChannels.remove(channelId)) {
        setError(error, QStringLiteral("The shared microG bridge disconnected during startup."));
        if (m_channels.isEmpty() && m_openingChannels.isEmpty()) stop();
        return false;
    }
    m_channels.insert(channelId);
    return true;
}

void SharedMicrogSupervisor::closeChannel(quint64 channelId)
{
    if (!m_channels.contains(channelId)) return;
    QString error;
    m_closedChannelIds.insert(channelId);
    m_closingChannels.insert(channelId);
    if (m_ready &&
        (!sendMessage(Close, channelId, 0, 0, QByteArray(), -1, &error) ||
         !waitForReply(Ack, channelId, &error))) {
        m_closingChannels.remove(channelId);
        failWorker();
        return;
    }
    m_channels.remove(channelId);
    m_closingChannels.remove(channelId);
    if (m_channels.isEmpty()) stop();
}

void SharedMicrogSupervisor::stop()
{
    if (m_pid <= 0) {
        m_reaper.stop();
        if (m_socketNotifier) {
            m_socketNotifier->setEnabled(false);
            delete m_socketNotifier;
            m_socketNotifier = nullptr;
        }
        closeControlSocket();
        m_ready = false;
        m_channels.clear();
        m_usedChannelIds.clear();
        m_closedChannelIds.clear();
        m_openingChannels.clear();
        m_closingChannels.clear();
        m_disconnectedChannels.clear();
        return;
    }
    m_stopping = true;
    if (m_ready) {
        QString error;
        if (sendMessage(Stop, 0, 0, 0, QByteArray(), -1, &error))
            waitForReply(Ack, 0, &error);
    }
    QElapsedTimer elapsed;
    elapsed.start();
    int status = 0;
    while (m_pid > 0 && elapsed.elapsed() < 1000) {
        const pid_t result = ::waitpid(m_pid, &status, WNOHANG);
        if (result == m_pid || (result < 0 && errno == ECHILD)) {
            m_pid = -1;
            break;
        }
        usleep(10000);
    }
    if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
        elapsed.restart();
        while (m_pid > 0 && elapsed.elapsed() < 500) {
            const pid_t result = ::waitpid(m_pid, &status, WNOHANG);
            if (result == m_pid || (result < 0 && errno == ECHILD)) {
                m_pid = -1;
                break;
            }
            usleep(10000);
        }
    }
    if (m_pid > 0) {
        ::kill(m_pid, SIGKILL);
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
        m_pid = -1;
    }
    m_stopping = false;
    m_reaper.stop();
    if (m_socketNotifier) {
        m_socketNotifier->setEnabled(false);
        delete m_socketNotifier;
        m_socketNotifier = nullptr;
    }
    closeControlSocket();
    m_ready = false;
    m_channels.clear();
    m_usedChannelIds.clear();
    m_closedChannelIds.clear();
    m_openingChannels.clear();
    m_closingChannels.clear();
    m_disconnectedChannels.clear();
}

bool SharedMicrogSupervisor::checkChild(bool reportDeath)
{
    if (m_pid <= 0) return false;
    int status = 0;
    const pid_t result = ::waitpid(m_pid, &status, WNOHANG);
    if (result == 0) return true;
    if (result < 0 && errno != ECHILD) return true;
    m_pid = -1;
    m_ready = false;
    m_channels.clear();
    m_usedChannelIds.clear();
    m_closedChannelIds.clear();
    m_openingChannels.clear();
    m_closingChannels.clear();
    m_disconnectedChannels.clear();
    m_reaper.stop();
    if (m_socketNotifier) {
        m_socketNotifier->setEnabled(false);
        delete m_socketNotifier;
        m_socketNotifier = nullptr;
    }
    closeControlSocket();
    if (reportDeath && !m_stopping) emit workerDied();
    return false;
}

void SharedMicrogSupervisor::failWorker()
{
    const bool wasActive = m_pid > 0 || m_ready || !m_channels.isEmpty() ||
                           !m_openingChannels.isEmpty();
    if (m_pid > 0) {
        ::kill(m_pid, SIGKILL);
        int status = 0;
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
        m_pid = -1;
    }
    m_ready = false;
    m_channels.clear();
    m_usedChannelIds.clear();
    m_closedChannelIds.clear();
    m_openingChannels.clear();
    m_closingChannels.clear();
    m_disconnectedChannels.clear();
    m_reaper.stop();
    if (m_socketNotifier) {
        m_socketNotifier->setEnabled(false);
        delete m_socketNotifier;
        m_socketNotifier = nullptr;
    }
    closeControlSocket();
    if (wasActive && !m_stopping) emit workerDied();
}

void SharedMicrogSupervisor::reapChild()
{
    checkChild(true);
}

void SharedMicrogSupervisor::closeControlSocket()
{
    if (m_controlFd >= 0) ::close(m_controlFd);
    m_controlFd = -1;
}

void SharedMicrogSupervisor::readUnsolicited()
{
    while (m_controlFd >= 0) {
        Message message;
        QString error;
        bool wouldBlock = false;
        if (!receiveMessage(&message, &error, &wouldBlock)) {
            if (wouldBlock) return;
            failWorker();
            return;
        }
        if (message.type != Disconnected || message.channelId == 0) {
            failWorker();
            return;
        }
        handleDisconnected(message.channelId);
    }
}

void SharedMicrogSupervisor::handleDisconnected(quint64 channelId)
{
    const bool known = m_channels.contains(channelId) || m_openingChannels.contains(channelId) ||
                       m_closingChannels.contains(channelId);
    if (!known && m_closedChannelIds.contains(channelId)) return;
    if (!known) {
        failWorker();
        return;
    }
    m_channels.remove(channelId);
    if (m_openingChannels.contains(channelId))
        m_disconnectedChannels.insert(channelId);
    if (!m_closingChannels.contains(channelId))
        emit channelDisconnected(channelId);
    if (m_channels.isEmpty() && m_openingChannels.isEmpty() && m_closingChannels.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_channels.isEmpty() && m_openingChannels.isEmpty() && m_closingChannels.isEmpty())
                stop();
        });
    }
}
