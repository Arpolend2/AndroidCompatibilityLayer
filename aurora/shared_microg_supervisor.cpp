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
const int MaxInputBytes = 4096;
const int FrameHeaderSize = 48;
const int MaxFrameChunkBytes = 60 * 1024;
const int MaxFrameBytes = 16 * 1024 * 1024;
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
    if (m_pid > 0 && m_ready && checkChild(true)) return true;
    if (!isConfigured()) {
        setError(error, QStringLiteral("The shared microG worker is not configured."));
        return false;
    }

    int controlSockets[2], frameSockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, controlSockets) != 0) {
        setError(error, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, frameSockets) != 0) {
        const int socketError = errno;
        ::close(controlSockets[0]);
        ::close(controlSockets[1]);
        setError(error, QString::fromLocal8Bit(std::strerror(socketError)));
        return false;
    }
    const int parentFd = ::fcntl(controlSockets[0], F_DUPFD_CLOEXEC, 5);
    const int childFd = ::fcntl(controlSockets[1], F_DUPFD_CLOEXEC, 5);
    const int parentFrameFd = ::fcntl(frameSockets[0], F_DUPFD_CLOEXEC, 5);
    const int childFrameFd = ::fcntl(frameSockets[1], F_DUPFD_CLOEXEC, 5);
    const int duplicateError = errno;
    for (int fd : controlSockets) ::close(fd);
    for (int fd : frameSockets) ::close(fd);
    if (parentFd < 0 || childFd < 0 || parentFrameFd < 0 || childFrameFd < 0) {
        if (parentFd >= 0) ::close(parentFd);
        if (childFd >= 0) ::close(childFd);
        if (parentFrameFd >= 0) ::close(parentFrameFd);
        if (childFrameFd >= 0) ::close(childFrameFd);
        setError(error, QString::fromLocal8Bit(std::strerror(duplicateError)));
        return false;
    }

    posix_spawn_file_actions_t actions;
    int status = ::posix_spawn_file_actions_init(&actions);
    const bool actionsInitialized = status == 0;
    if (status == 0) status = ::posix_spawn_file_actions_adddup2(&actions, childFd, 3);
    if (status == 0) status = ::posix_spawn_file_actions_adddup2(&actions, childFrameFd, 4);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, parentFd);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, childFd);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, parentFrameFd);
    if (status == 0) status = ::posix_spawn_file_actions_addclose(&actions, childFrameFd);

    QList<QByteArray> encodedArguments;
    encodedArguments << m_program.toLocal8Bit()
                     << QByteArrayLiteral("--shared-microg-worker")
                     << QByteArrayLiteral("--headless")
                     << QByteArrayLiteral("--control-fd=3")
                     << QByteArrayLiteral("--frame-fd=4")
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
    ::close(childFrameFd);
    if (status != 0) {
        ::close(parentFd);
        ::close(parentFrameFd);
        m_pid = -1;
        setError(error, QString::fromLocal8Bit(std::strerror(status)));
        return false;
    }

    m_controlFd = parentFd;
    m_frameFd = parentFrameFd;
    const int flags = ::fcntl(m_controlFd, F_GETFL);
    const int frameFlags = ::fcntl(m_frameFd, F_GETFL);
    if (flags < 0 || ::fcntl(m_controlFd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        frameFlags < 0 || ::fcntl(m_frameFd, F_SETFL, frameFlags | O_NONBLOCK) != 0) {
        setError(error, QStringLiteral("Cannot configure the shared microG worker sockets."));
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
    m_frameNotifier = new QSocketNotifier(m_frameFd, QSocketNotifier::Read, this);
    connect(m_frameNotifier, &QSocketNotifier::activated, this,
            [this](int) { readFrames(); });
    m_reaper.start();
    return true;
}

bool SharedMicrogSupervisor::startRetained(QString *error)
{
    m_retainWithoutClients = true;
    return start(error);
}

void SharedMicrogSupervisor::releaseRetention()
{
    m_retainWithoutClients = false;
    if (m_channels.isEmpty() && m_openingChannels.isEmpty() && m_closingChannels.isEmpty())
        stop();
}

bool SharedMicrogSupervisor::sendMessage(MessageType type, quint64 channelId, quint32 uid, quint32 pid,
                                     const QByteArray &package, int fd, QString *error)
{
    const bool open = type == Open;
    const bool close = type == Close && channelId != 0;
    const bool stop = type == Stop && channelId == 0;
    const bool input = type == Touch || type == Back || type == TextCommit ||
                       type == TextKey || type == KeyboardClosed || type == KeyboardHeight ||
                       type == UiResize;
    if (m_controlFd < 0 || (!open && !close && !stop && !input) ||
        package.size() > (input ? MaxInputBytes : MaxPackageBytes) ||
        (open ? (channelId == 0 || fd < 0 || package.isEmpty() || uid == 0 || pid == 0)
              : (fd >= 0 || (!input && !package.isEmpty()) ||
                 (!open && !input && (uid != 0 || pid != 0)))) ||
        (input && ((!m_channels.contains(channelId) &&
                    !(type == UiResize && m_openingChannels.contains(channelId))) ||
                   !channelId || fd >= 0))) {
        setError(error, QStringLiteral("Invalid shared microG control message."));
        return false;
    }
    if (type == Touch && (uid > 2 || pid > 15 || package.size() != 8)) {
        setError(error, QStringLiteral("Invalid shared microG touch event."));
        return false;
    }
    if (type == Back && (uid || pid || !package.isEmpty())) {
        setError(error, QStringLiteral("Invalid shared microG Back event."));
        return false;
    }
    if (type == TextCommit && (uid || pid || package.contains('\0'))) {
        setError(error, QStringLiteral("Invalid shared microG text event."));
        return false;
    }
    if (type == TextKey && (uid < 1 || uid > 9 || pid || !package.isEmpty())) {
        setError(error, QStringLiteral("Invalid shared microG text key."));
        return false;
    }
    if (type == KeyboardClosed && (uid || pid || !package.isEmpty())) {
        setError(error, QStringLiteral("Invalid shared microG keyboard event."));
        return false;
    }
    if (type == KeyboardHeight && (uid > 8192 || pid || !package.isEmpty())) {
        setError(error, QStringLiteral("Invalid shared microG keyboard height."));
        return false;
    }
    if (type == UiResize && (!uid || uid > 2048 || !pid || pid > 2048 ||
                             package.size() != 8 || !read32(package.constData()) ||
                             read32(package.constData()) > 8192 ||
                             read32(package.constData() + 4) > 3)) {
        setError(error, QStringLiteral("Invalid shared microG UI size."));
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

bool SharedMicrogSupervisor::sendTouch(quint64 channelId, int phase, int pointer, int x, int y)
{
    if (phase < 0 || phase > 2 || pointer < 0 || pointer > 15 || x < 0 || x > 8192 || y < 0 || y > 8192)
        return false;
    QByteArray coordinates(8, '\0');
    write32(coordinates.data(), static_cast<quint32>(x));
    write32(coordinates.data() + 4, static_cast<quint32>(y));
    return sendMessage(Touch, channelId, static_cast<quint32>(phase),
                       static_cast<quint32>(pointer), coordinates, -1, nullptr);
}

bool SharedMicrogSupervisor::sendBack(quint64 channelId)
{
    return sendMessage(Back, channelId, 0, 0, QByteArray(), -1, nullptr);
}

bool SharedMicrogSupervisor::sendText(quint64 channelId, const QByteArray &text)
{
    return !text.isEmpty() && text.size() <= MaxInputBytes &&
           sendMessage(TextCommit, channelId, 0, 0, text, -1, nullptr);
}

bool SharedMicrogSupervisor::sendTextKey(quint64 channelId, int key)
{
    return sendMessage(TextKey, channelId, static_cast<quint32>(key), 0,
                       QByteArray(), -1, nullptr);
}

bool SharedMicrogSupervisor::sendKeyboardClosed(quint64 channelId)
{
    return sendMessage(KeyboardClosed, channelId, 0, 0, QByteArray(), -1, nullptr);
}

bool SharedMicrogSupervisor::sendKeyboardHeight(quint64 channelId, int height)
{
    return height >= 0 && height <= 8192 &&
           sendMessage(KeyboardHeight, channelId, static_cast<quint32>(height),
                       0, QByteArray(), -1, nullptr);
}

bool SharedMicrogSupervisor::sendUiResize(quint64 channelId, int width, int height,
                                         int density, int rotation)
{
    if (channelId != m_authUiChannel || width <= 0 || width > 2048 ||
        height <= 0 || height > 2048 || density <= 0 || density > 8192 ||
        rotation < 0 || rotation > 3)
        return false;
    QByteArray dimensions(8, '\0');
    write32(dimensions.data(), static_cast<quint32>(density));
    write32(dimensions.data() + 4, static_cast<quint32>(rotation));
    return sendMessage(UiResize, channelId, static_cast<quint32>(width),
                       static_cast<quint32>(height), dimensions, -1, nullptr);
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
    const bool simple = message->type == Ready || message->type == Ack ||
                        message->type == Nack || message->type == Disconnected ||
                        message->type == UiStarted || message->type == UiFinished ||
                        message->type == KeyboardHide;
    const bool frameDiagnostic = message->type == FrameDelivered ||
                                 message->type == FrameSuppressed ||
                                 message->type == FrameSendFailed;
    const bool keyboard = message->type == KeyboardShow &&
                          message->uid <= 7 && message->pid <= 255 && message->packageLength <= 6;
    if ((!simple && !keyboard && !frameDiagnostic) ||
        (simple && (message->uid || message->pid || message->packageLength)) ||
        (frameDiagnostic && (!message->channelId ||
                             (message->type == FrameDelivered &&
                              (message->uid > 2048 || message->pid > 2048 ||
                               message->packageLength != 0)) ||
                             (message->type == FrameSuppressed &&
                              (message->uid > 2048 || message->pid > 2048 ||
                               message->packageLength != 0)) ||
                             (message->type == FrameSendFailed &&
                              (message->uid > MaxFrameBytes || message->pid > MaxFrameBytes ||
                               message->packageLength != 0)))) ||
        (message->type == Disconnected && !message->channelId) ||
        ((message->type == UiStarted || message->type == UiFinished ||
          message->type == KeyboardShow || message->type == KeyboardHide) && !message->channelId)) {
        setError(error, QStringLiteral("Unexpected shared microG worker response."));
        return false;
    }
    return true;
}

bool SharedMicrogSupervisor::receiveFrame(QImage *frame, quint64 *channelId,
                                          QString *error, bool *wouldBlock)
{
    if (wouldBlock) *wouldBlock = false;
    QByteArray packet(FrameHeaderSize + MaxFrameChunkBytes, Qt::Uninitialized);
    const ssize_t count = ::recv(m_frameFd, packet.data(), static_cast<size_t>(packet.size()),
                                 MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (wouldBlock) *wouldBlock = true;
        return false;
    }
    if (count < FrameHeaderSize || count > packet.size() ||
        std::memcmp(packet.constData(), "MGUI", 4) != 0 ||
        read32(packet.constData() + 4) != 1) {
        setError(error, QStringLiteral("Invalid shared microG UI frame packet."));
        return false;
    }
    const quint64 channel = read64(packet.constData() + 8);
    const quint64 sequence = read64(packet.constData() + 16);
    const quint32 width = read32(packet.constData() + 24);
    const quint32 height = read32(packet.constData() + 28);
    const quint32 stride = read32(packet.constData() + 32);
    const quint32 total = read32(packet.constData() + 36);
    const quint32 offset = read32(packet.constData() + 40);
    const quint32 chunk = read32(packet.constData() + 44);
    if (!channel || !sequence || width == 0 || width > 2048 || height == 0 || height > 2048 ||
        stride < width * 4 || stride > width * 4 + 4096 ||
        total != stride * height || total > MaxFrameBytes ||
        chunk == 0 || chunk > MaxFrameChunkBytes ||
        static_cast<quint32>(count) != FrameHeaderSize + chunk ||
        offset > total || chunk > total - offset) {
        setError(error, QStringLiteral("Invalid shared microG UI frame dimensions."));
        return false;
    }
    if (offset == 0) {
        if (sequence <= m_frameSequence) {
            setError(error, QStringLiteral("Out-of-order shared microG UI frame."));
            return false;
        }
        m_frameSequence = sequence;
        m_frameChannel = channel;
        m_frameWidth = static_cast<int>(width);
        m_frameHeight = static_cast<int>(height);
        m_frameStride = static_cast<int>(stride);
        m_frameOffset = 0;
        m_frameBuffer.resize(static_cast<int>(total));
    }
    if (sequence != m_frameSequence || channel != m_frameChannel ||
        width != static_cast<quint32>(m_frameWidth) ||
        height != static_cast<quint32>(m_frameHeight) ||
        stride != static_cast<quint32>(m_frameStride) || offset != m_frameOffset) {
        m_frameBuffer.clear();
        m_frameOffset = 0;
        setError(error, QStringLiteral("Inconsistent shared microG UI frame chunks."));
        return false;
    }
    std::memcpy(m_frameBuffer.data() + offset, packet.constData() + FrameHeaderSize, chunk);
    m_frameOffset += chunk;
    if (m_frameOffset != total) return true;
    QImage decoded(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32_Premultiplied);
    if (decoded.isNull()) {
        m_frameBuffer.clear();
        m_frameOffset = 0;
        setError(error, QStringLiteral("Cannot allocate shared microG UI frame."));
        return false;
    }
    for (int row = 0; row < static_cast<int>(height); ++row)
        std::memcpy(decoded.scanLine(row), m_frameBuffer.constData() + row * stride,
                    static_cast<size_t>(width) * 4);
    m_frameBuffer.clear();
    m_frameOffset = 0;
    *frame = decoded;
    *channelId = channel;
    if (sequence == 1 || sequence % 300 == 0)
        qInfo("Shared microG UI frame assembled: %dx%d, stride=%d, sequence=%llu",
              width, height, stride, static_cast<unsigned long long>(sequence));
    return true;
}

void SharedMicrogSupervisor::handleWorkerEvent(const Message &message)
{
    if (message.type == UiStarted)
        qInfo("Shared microG UI started: channel=%llu",
              static_cast<unsigned long long>(message.channelId));
    else if (message.type == UiFinished)
        qInfo("Shared microG UI finished: channel=%llu",
              static_cast<unsigned long long>(message.channelId));
    if (message.type == FrameDelivered) {
        qInfo("Shared microG UI frame delivered: channel=%llu, size=%ux%u",
              static_cast<unsigned long long>(message.channelId),
              message.uid, message.pid);
    } else if (message.type == FrameSuppressed) {
        qInfo("Shared microG UI frame suppressed: channel=%llu, size=%ux%u",
              static_cast<unsigned long long>(message.channelId),
              message.uid, message.pid);
    } else if (message.type == FrameSendFailed) {
        qInfo("Shared microG UI frame send failed: channel=%llu, bytes=%u/%u",
              static_cast<unsigned long long>(message.channelId),
              message.uid, message.pid);
    }
    if (message.type == UiStarted || message.type == UiFinished) {
        const bool active = message.type == UiStarted;
        if (!m_channels.contains(message.channelId) && !m_openingChannels.contains(message.channelId)) {
            failWorker();
            return;
        }
        if (active && m_authUiChannel && m_authUiChannel != message.channelId) {
            failWorker();
            return;
        }
        if (active) m_authUiChannel = message.channelId;
        else if (m_authUiChannel == message.channelId) m_authUiChannel = 0;
        if (!active && m_frameChannel == message.channelId) {
            m_frameChannel = 0;
            m_frameBuffer.clear();
        }
        emit authenticationUiChanged(message.channelId, active);
        if (active && m_lastFrameChannel == message.channelId && !m_lastFrame.isNull())
            emit authenticationFrameChanged(message.channelId, m_lastFrame);
        if (!active && m_lastFrameChannel == message.channelId) {
            m_lastFrameChannel = 0;
            m_lastFrame = QImage();
        }
    } else if (message.type == KeyboardShow) {
        if (m_channels.contains(message.channelId))
            emit authenticationKeyboardRequested(message.channelId,
                static_cast<int>(message.uid), static_cast<int>(message.pid),
                static_cast<int>(message.packageLength));
    } else if (message.type == KeyboardHide) {
        if (m_channels.contains(message.channelId))
            emit authenticationKeyboardHidden(message.channelId);
    }
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
        if (reply.type == UiStarted || reply.type == UiFinished ||
            reply.type == KeyboardShow || reply.type == KeyboardHide ||
            reply.type == FrameDelivered || reply.type == FrameSuppressed ||
            reply.type == FrameSendFailed) {
            handleWorkerEvent(reply);
            if (m_controlFd < 0) {
                setError(error, QStringLiteral("The shared microG worker stopped."));
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
            if (!m_retainWithoutClients && m_channels.isEmpty() && m_openingChannels.isEmpty()) stop();
            return false;
        }
        failWorker();
        return false;
    }
    m_openingChannels.remove(channelId);
    if (m_disconnectedChannels.remove(channelId)) {
        setError(error, QStringLiteral("The shared microG bridge disconnected during startup."));
        if (!m_retainWithoutClients && m_channels.isEmpty() && m_openingChannels.isEmpty()) stop();
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
    if (!m_retainWithoutClients && m_channels.isEmpty()) stop();
}

void SharedMicrogSupervisor::stop()
{
    if (m_pid <= 0) {
        if (m_authUiChannel) {
            const quint64 channel = m_authUiChannel;
            m_authUiChannel = 0;
            emit authenticationUiChanged(channel, false);
        }
        m_reaper.stop();
        if (m_socketNotifier) {
            m_socketNotifier->setEnabled(false);
            delete m_socketNotifier;
            m_socketNotifier = nullptr;
        }
        if (m_frameNotifier) {
            m_frameNotifier->setEnabled(false);
            delete m_frameNotifier;
            m_frameNotifier = nullptr;
        }
        closeControlSocket();
        closeFrameSocket();
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
    if (m_frameNotifier) {
        m_frameNotifier->setEnabled(false);
        delete m_frameNotifier;
        m_frameNotifier = nullptr;
    }
    if (m_authUiChannel) {
        const quint64 channel = m_authUiChannel;
        m_authUiChannel = 0;
        emit authenticationUiChanged(channel, false);
    }
    closeControlSocket();
    closeFrameSocket();
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
    if (m_frameNotifier) {
        m_frameNotifier->setEnabled(false);
        delete m_frameNotifier;
        m_frameNotifier = nullptr;
    }
    closeControlSocket();
    closeFrameSocket();
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
    if (m_frameNotifier) {
        m_frameNotifier->setEnabled(false);
        delete m_frameNotifier;
        m_frameNotifier = nullptr;
    }
    closeControlSocket();
    closeFrameSocket();
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

void SharedMicrogSupervisor::closeFrameSocket()
{
    if (m_frameFd >= 0) ::close(m_frameFd);
    m_frameFd = -1;
    m_frameBuffer.clear();
    m_frameOffset = 0;
    m_frameChannel = 0;
    m_authUiChannel = 0;
    m_lastFrameChannel = 0;
    m_lastFrame = QImage();
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
        if (message.type == UiStarted || message.type == UiFinished ||
            message.type == KeyboardShow || message.type == KeyboardHide ||
            message.type == FrameDelivered || message.type == FrameSuppressed ||
            message.type == FrameSendFailed) {
            handleWorkerEvent(message);
            if (m_controlFd < 0) return;
            continue;
        }
        if (message.type != Disconnected || message.channelId == 0) {
            failWorker();
            return;
        }
        handleDisconnected(message.channelId);
    }
}

void SharedMicrogSupervisor::readFrames()
{
    const int maxPacketsPerDispatch = 16;
    for (int packets = 0; m_frameFd >= 0 && packets < maxPacketsPerDispatch; ++packets) {
        QImage frame;
        quint64 channel = 0;
        QString error;
        bool wouldBlock = false;
        if (!receiveFrame(&frame, &channel, &error, &wouldBlock)) {
            if (wouldBlock) return;
            qWarning("Shared microG UI frame rejected: %s", qPrintable(error));
            failWorker();
            return;
        }
        if (frame.isNull()) continue;
        if (!m_channels.contains(channel) && !m_openingChannels.contains(channel)) {
            qWarning("Shared microG UI frame has no active client channel.");
            failWorker();
            return;
        }
        m_lastFrameChannel = channel;
        m_lastFrame = frame;
        emit authenticationFrameChanged(channel, frame);
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
    if (m_authUiChannel == channelId) {
        m_authUiChannel = 0;
        m_frameBuffer.clear();
        m_frameOffset = 0;
        m_lastFrameChannel = 0;
        m_lastFrame = QImage();
        emit authenticationUiChanged(channelId, false);
    }
    m_channels.remove(channelId);
    if (m_openingChannels.contains(channelId))
        m_disconnectedChannels.insert(channelId);
    if (!m_closingChannels.contains(channelId))
        emit channelDisconnected(channelId);
    if (!m_retainWithoutClients && m_channels.isEmpty() && m_openingChannels.isEmpty() && m_closingChannels.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_retainWithoutClients && m_channels.isEmpty() && m_openingChannels.isEmpty() && m_closingChannels.isEmpty())
                stop();
        });
    }
}
