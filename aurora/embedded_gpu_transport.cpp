#include "embedded_gpu_transport.h"

#include <QAtomicInteger>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QMutexLocker>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if __has_include(<gbm.h>)
#include <gbm.h>
#define ACL_HAVE_GBM_HEADER 1
#endif

namespace {
const quint32 wireMagic = 0x41434c47;
const quint16 wireVersion = 2;
const int maxPlanes = 4;
const int maxDimension = 4096;
const quint64 maxFramePixels = (32 * 1024 * 1024) / 4;
const quint64 invalidModifier = 0x00ffffffffffffffULL;

enum WireType {
    FramePacket = 1,
    ResultPacket = 2,
    ProbePacket = 3
};

quint64 nextFrameId()
{
    static QAtomicInteger<quint64> next(1);
    quint64 id = next.fetchAndAddRelaxed(1);
    return id ? id : next.fetchAndAddRelaxed(1);
}

#pragma pack(push, 1)
struct WirePacket {
    quint32 magic;
    quint16 version;
    quint16 type;
    quint64 id;
    quint64 channel;
    quint64 generation;
    qint32 width;
    qint32 height;
    qint32 fourcc;
    quint32 planeCount;
    quint64 modifier;
    qint32 strides[maxPlanes];
    qint32 offsets[maxPlanes];
    qint32 result;
};
#pragma pack(pop)

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE1_FD_EXT
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#endif
#ifndef EGL_DMA_BUF_PLANE2_FD_EXT
#define EGL_DMA_BUF_PLANE2_FD_EXT 0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT 0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT 0x327A
#endif
#ifndef EGL_DMA_BUF_PLANE3_FD_EXT
#define EGL_DMA_BUF_PLANE3_FD_EXT 0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT 0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT 0x3442
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#endif
#ifndef EGL_DRM_FORMAT_MODIFIER_INVALID_EXT
#define EGL_DRM_FORMAT_MODIFIER_INVALID_EXT 0x00ffffffffffffffULL
#endif

typedef EGLImageKHR (*CreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*DestroyImage)(EGLDisplay, EGLImageKHR);
typedef EGLBoolean (*ExportQuery)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLuint64KHR *);
typedef EGLBoolean (*ExportImage)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLint *);
typedef EGLBoolean (*QueryFormats)(EGLDisplay, EGLint, EGLint *, EGLint *);
typedef EGLBoolean (*QueryModifiers)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *);
typedef void (GL_APIENTRYP ImageTargetTexture)(GLenum, EGLImageKHR);

bool hasExtension(const char *extensions, const char *name)
{
    if (!extensions || !name || !*name) return false;
    const size_t length = std::strlen(name);
    const char *p = extensions;
    while ((p = std::strstr(p, name)) != nullptr) {
        if ((p == extensions || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0'))
            return true;
        p += length;
    }
    return false;
}

bool hasGlExtension(const char *name)
{
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    if (!version) return false;
    if (std::strstr(version, "OpenGL ES 3")) {
        typedef const GLubyte *(GL_APIENTRYP GetStringi)(GLenum, GLuint);
        GetStringi getStringi = reinterpret_cast<GetStringi>(eglGetProcAddress("glGetStringi"));
        GLint count = 0;
        if (!getStringi) return false;
        glGetIntegerv(0x821D, &count);
        if (count < 0 || count > 65536) return false;
        for (GLint i = 0; i < count; ++i) {
            const char *extension = reinterpret_cast<const char *>(getStringi(GL_EXTENSIONS, GLuint(i)));
            if (extension && std::strcmp(extension, name) == 0) return true;
        }
        return false;
    }
    return hasExtension(reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)), name);
}

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool waitFd(int fd, short events, int timeoutMs)
{
    pollfd descriptor = {fd, events, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & events) && !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL));
}

bool sendPacket(int fd, const WirePacket &packet, const int *fds, int count)
{
    iovec iov = {const_cast<WirePacket *>(&packet), sizeof(packet)};
    char control[CMSG_SPACE(sizeof(int) * maxPlanes)] = {};
    msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (count) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * count);
        cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * count);
        std::memcpy(CMSG_DATA(header), fds, sizeof(int) * count);
    }
    ssize_t sent;
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFd(fd, POLLOUT, 1000))
            continue;
        break;
    } while (true);
    return sent == ssize_t(sizeof(packet));
}

bool receivePacket(int fd, WirePacket *packet, int *fds, int *fdCount)
{
    char control[CMSG_SPACE(sizeof(int) * maxPlanes)] = {};
    iovec iov = {packet, sizeof(*packet)};
    msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (received < 0)
        return false;
    if (received == 0) {
        errno = ECONNRESET;
        return false;
    }
    int count = 0;
    bool malformed = false;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            malformed = true;
            continue;
        }
        const size_t bytes = header->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) {
            malformed = true;
            continue;
        }
        const int *receivedFds = reinterpret_cast<const int *>(CMSG_DATA(header));
        const int n = int(bytes / sizeof(int));
        for (int i = 0; i < n; ++i) {
            if (count < maxPlanes)
                fds[count++] = receivedFds[i];
            else {
                close(receivedFds[i]);
                malformed = true;
            }
        }
    }
    if (malformed) {
        for (int i = 0; i < count; ++i) close(fds[i]);
        errno = EPROTO;
        return false;
    }
    if (received != ssize_t(sizeof(*packet)) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        for (int i = 0; i < count; ++i) close(fds[i]);
        errno = EPROTO;
        return false;
    }
    *fdCount = count;
    return true;
}

bool validFrameMetadata(const WirePacket &packet, int fdCount)
{
    if (packet.magic != wireMagic || packet.version != wireVersion ||
        (packet.type != FramePacket && packet.type != ProbePacket) || !packet.id || packet.width <= 0 ||
        packet.height <= 0 || packet.width > maxDimension || packet.height > maxDimension ||
        quint64(packet.width) * quint64(packet.height) > maxFramePixels ||
        packet.fourcc <= 0 || !packet.planeCount || packet.planeCount > maxPlanes ||
        fdCount != int(packet.planeCount) || bool(packet.channel) != bool(packet.generation))
        return false;
    for (uint32_t i = 0; i < packet.planeCount; ++i)
        if (packet.strides[i] <= 0 || packet.offsets[i] < 0)
            return false;
    return true;
}

bool modifierSupported(EGLDisplay display, EGLint format, quint64 modifier, QString *error)
{
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import")) {
        setError(error, QStringLiteral("consumer lacks EGL_EXT_image_dma_buf_import"));
        return false;
    }
    QueryFormats queryFormats = reinterpret_cast<QueryFormats>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    if (!queryFormats) {
        setError(error, QStringLiteral("eglQueryDmaBufFormatsEXT is unavailable"));
        return false;
    }
    EGLint formatCount = 0;
    if (!queryFormats(display, 0, nullptr, &formatCount) || formatCount <= 0 || formatCount > 4096) {
        setError(error, QStringLiteral("consumer could not query DMA-BUF formats"));
        return false;
    }
    std::vector<EGLint> formats(static_cast<size_t>(formatCount));
    EGLint actualFormats = 0;
    if (!queryFormats(display, formatCount, formats.data(), &actualFormats) ||
        actualFormats < 0 || actualFormats > formatCount ||
        std::find(formats.begin(), formats.begin() + actualFormats, format) == formats.begin() + actualFormats) {
        setError(error, QStringLiteral("consumer does not support the exported DMA-BUF format"));
        return false;
    }
    if (modifier == invalidModifier) {
        if (hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers"))
            return true;
        return true;
    }
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
        setError(error, QStringLiteral("producer exported a modifier unsupported by the consumer"));
        return false;
    }
    QueryModifiers query = reinterpret_cast<QueryModifiers>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!query) {
        setError(error, QStringLiteral("eglQueryDmaBufModifiersEXT is unavailable"));
        return false;
    }
    EGLint count = 0;
    if (!query(display, format, 0, nullptr, nullptr, &count) || count <= 0 || count > 4096) {
        setError(error, QStringLiteral("consumer could not query DMA-BUF modifiers"));
        return false;
    }
    std::vector<EGLuint64KHR> modifiers(static_cast<size_t>(count));
    std::vector<EGLBoolean> externalOnly(static_cast<size_t>(count));
    EGLint actual = 0;
    if (!query(display, format, count, modifiers.data(), externalOnly.data(), &actual) ||
        actual < 0 || actual > count) {
        setError(error, QStringLiteral("consumer DMA-BUF modifier query failed"));
        return false;
    }
    for (EGLint i = 0; i < actual; ++i) {
        if (modifiers[size_t(i)] == EGLuint64KHR(modifier)) {
            if (externalOnly[size_t(i)]) {
                setError(error, QStringLiteral("consumer supports this modifier only as an external texture"));
                return false;
            }
            return true;
        }
    }
    setError(error, QStringLiteral("consumer does not support the exported DMA-BUF modifier"));
    return false;
}
}

AuroraDmaBufFrame::AuroraDmaBufFrame(AuroraDmaBufFrame &&other) noexcept
{
    *this = std::move(other);
}

AuroraDmaBufFrame &AuroraDmaBufFrame::operator=(AuroraDmaBufFrame &&other) noexcept
{
    if (this == &other) return *this;
    close();
    id = other.id;
    channel = other.channel;
    generation = other.generation;
    probeOnly = other.probeOnly;
    width = other.width;
    height = other.height;
    fourcc = other.fourcc;
    planeCount = other.planeCount;
    modifier = other.modifier;
    for (int i = 0; i < maxPlanes; ++i) {
        fds[i] = other.fds[i];
        strides[i] = other.strides[i];
        offsets[i] = other.offsets[i];
        other.fds[i] = -1;
    }
    other.id = 0;
    other.channel = other.generation = 0;
    other.planeCount = 0;
    return *this;
}

AuroraDmaBufFrame::~AuroraDmaBufFrame()
{
    close();
}

void AuroraDmaBufFrame::close()
{
    for (int &fd : fds) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    planeCount = 0;
}

EmbeddedGpuTransport::EmbeddedGpuTransport(QObject *parent) : QObject(parent) {}

EmbeddedGpuTransport::~EmbeddedGpuTransport()
{
    close();
}

bool EmbeddedGpuTransport::listen(const QString &path, qint64 expectedPeerPid, QString *error)
{
    close();
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.isEmpty() || encoded.size() >= int(sizeof(sockaddr_un::sun_path))) {
        setError(error, tr("The GPU channel path is invalid."));
        return false;
    }
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        setError(error, tr("Cannot create the GPU channel: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()) + 1);
    unlink(address.sun_path);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        chmod(address.sun_path, S_IRUSR | S_IWUSR) != 0 || ::listen(fd, 1) != 0) {
        const int saved = errno;
        ::close(fd);
        unlink(address.sun_path);
        setError(error, tr("Cannot listen on the GPU channel: %1").arg(QString::fromLocal8Bit(std::strerror(saved))));
        return false;
    }
    m_listener = fd;
    m_path = path;
    m_expectedPeerPid = expectedPeerPid;
    m_server = true;
    m_listenerNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_listenerNotifier, &QSocketNotifier::activated, this, &EmbeddedGpuTransport::acceptPending);
    return true;
}

bool EmbeddedGpuTransport::connectTo(const QString &path, qint64 expectedPeerPid, QString *error)
{
    close();
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.isEmpty() || encoded.size() >= int(sizeof(sockaddr_un::sun_path))) {
        setError(error, tr("The GPU channel path is invalid."));
        return false;
    }
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        setError(error, tr("Cannot create the GPU channel: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()) + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        if (saved != EINPROGRESS || !waitFd(fd, POLLOUT, 1000)) {
            ::close(fd);
            setError(error, tr("Cannot connect to the GPU channel: %1").arg(QString::fromLocal8Bit(std::strerror(saved))));
            return false;
        }
        int socketError = 0;
        socklen_t errorSize = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0 || socketError) {
            if (socketError) errno = socketError;
            const int connectError = errno;
            ::close(fd);
            setError(error, tr("Cannot connect to the GPU channel: %1").arg(QString::fromLocal8Bit(std::strerror(connectError))));
            return false;
        }
    }
    m_server = false;
    return setPeer(fd, expectedPeerPid, error);
}

bool EmbeddedGpuTransport::adoptInheritedSocket(int fd, bool receiveFrames, QString *error)
{
    close();
    int type = 0;
    socklen_t typeSize = sizeof(type);
    sockaddr_storage local = {}, peer = {};
    socklen_t localSize = sizeof(local), peerSize = sizeof(peer);
    const int flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    if (fd < 0 || flags < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeSize) != 0 ||
        type != SOCK_SEQPACKET ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&local), &localSize) != 0 ||
        getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peerSize) != 0 ||
        local.ss_family != AF_UNIX || peer.ss_family != AF_UNIX ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (fd >= 0) ::close(fd);
        setError(error, tr("The inherited GPU channel is invalid."));
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_peer = fd;
    }
    if (receiveFrames) {
        m_peerNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(m_peerNotifier, &QSocketNotifier::activated, this, &EmbeddedGpuTransport::readAvailable);
    }
    return true;
}

bool EmbeddedGpuTransport::setPeer(int fd, qint64 expectedPeerPid, QString *error)
{
    struct ucred credentials = {};
    socklen_t size = sizeof(credentials);
    if (expectedPeerPid <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        size != sizeof(credentials) || credentials.pid != expectedPeerPid ||
        credentials.uid != getuid()) {
        ::close(fd);
        setError(error, tr("The GPU channel peer could not be authenticated."));
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        closePeerLocked();
        m_peer = fd;
        m_expectedPeerPid = expectedPeerPid;
    }
    emit peerConnected();
    return true;
}

void EmbeddedGpuTransport::setExpectedPeerPid(qint64 pid)
{
    m_expectedPeerPid = pid;
}

void EmbeddedGpuTransport::acceptPending()
{
    if (m_listener < 0) return;
    const int fd = accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            fail(tr("Cannot accept the GPU channel peer."));
        return;
    }
    if (connected()) {
        ::close(fd);
        return;
    }
    QString error;
    if (!setPeer(fd, m_expectedPeerPid, &error)) {
        fail(error);
        return;
    }
    m_peerNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_peerNotifier, &QSocketNotifier::activated, this, &EmbeddedGpuTransport::readAvailable);
}

void EmbeddedGpuTransport::readAvailable()
{
    const int fd = duplicatePeer();
    if (fd < 0) return;
    AuroraDmaBufFrame frame;
    errno = 0;
    const bool received = receiveFramePacket(fd, &frame);
    const int saved = errno;
    ::close(fd);
    if (!received) {
        if (saved == EAGAIN || saved == EWOULDBLOCK) return;
        if (saved == ECONNRESET || saved == ENOTCONN || saved == EPIPE) {
            closePeer();
            emit peerDisconnected();
            return;
        }
        fail(tr("The GPU channel sent an invalid packet."));
        return;
    }
    if (m_frameHandler)
        m_frameHandler(std::move(frame));
    else {
        frame.close();
        fail(tr("The GPU channel has no frame consumer."));
    }
}

void EmbeddedGpuTransport::closePeerLocked()
{
    if (m_peerNotifier) {
        m_peerNotifier->setEnabled(false);
        m_peerNotifier->deleteLater();
        m_peerNotifier = nullptr;
    }
    if (m_peer >= 0) ::close(m_peer);
    m_peer = -1;
}

void EmbeddedGpuTransport::closePeer()
{
    QMutexLocker lock(&m_mutex);
    closePeerLocked();
}

void EmbeddedGpuTransport::close()
{
    {
        QMutexLocker lock(&m_mutex);
        closePeerLocked();
        if (m_listenerNotifier) {
            m_listenerNotifier->setEnabled(false);
            m_listenerNotifier->deleteLater();
            m_listenerNotifier = nullptr;
        }
        if (m_listener >= 0) ::close(m_listener);
        m_listener = -1;
    }
    if (!m_path.isEmpty()) {
        const QByteArray encoded = QFile::encodeName(m_path);
        unlink(encoded.constData());
        m_path.clear();
    }
}

bool EmbeddedGpuTransport::connected() const
{
    QMutexLocker lock(&m_mutex);
    return m_peer >= 0;
}

void EmbeddedGpuTransport::setFrameHandler(const std::function<void(AuroraDmaBufFrame &&)> &handler)
{
    m_frameHandler = handler;
}

bool EmbeddedGpuTransport::sendFramePacket(int socket, const AuroraDmaBufFrame &frame)
{
    if (socket < 0 || !frame.id || frame.width <= 0 || frame.height <= 0 ||
        frame.width > maxDimension || frame.height > maxDimension ||
        quint64(frame.width) * quint64(frame.height) > maxFramePixels ||
        frame.fourcc <= 0 || frame.planeCount <= 0 || frame.planeCount > maxPlanes ||
        bool(frame.channel) != bool(frame.generation))
        return false;
    WirePacket packet = {};
    packet.magic = wireMagic;
    packet.version = wireVersion;
    packet.type = frame.probeOnly ? ProbePacket : FramePacket;
    packet.id = frame.id;
    packet.channel = frame.channel;
    packet.generation = frame.generation;
    packet.width = frame.width;
    packet.height = frame.height;
    packet.fourcc = frame.fourcc;
    packet.planeCount = quint32(frame.planeCount);
    packet.modifier = frame.modifier;
    for (int i = 0; i < frame.planeCount; ++i) {
        if (frame.fds[i] < 0 || frame.strides[i] <= 0 || frame.offsets[i] < 0)
            return false;
        packet.strides[i] = frame.strides[i];
        packet.offsets[i] = frame.offsets[i];
    }
    return sendPacket(socket, packet, frame.fds, frame.planeCount);
}

bool EmbeddedGpuTransport::receiveFramePacket(int socket, AuroraDmaBufFrame *frame)
{
    if (!frame) return false;
    WirePacket packet = {};
    int receivedFds[maxPlanes] = {-1, -1, -1, -1};
    int fdCount = 0;
    if (!receivePacket(socket, &packet, receivedFds, &fdCount)) return false;
    if (!validFrameMetadata(packet, fdCount)) {
        for (int i = 0; i < fdCount; ++i) ::close(receivedFds[i]);
        errno = EPROTO;
        return false;
    }
    AuroraDmaBufFrame incoming;
    incoming.id = packet.id;
    incoming.channel = packet.channel;
    incoming.generation = packet.generation;
    incoming.probeOnly = packet.type == ProbePacket;
    incoming.width = packet.width;
    incoming.height = packet.height;
    incoming.fourcc = packet.fourcc;
    incoming.planeCount = int(packet.planeCount);
    incoming.modifier = packet.modifier;
    for (int i = 0; i < incoming.planeCount; ++i) {
        incoming.fds[i] = receivedFds[i];
        incoming.strides[i] = packet.strides[i];
        incoming.offsets[i] = packet.offsets[i];
    }
    *frame = std::move(incoming);
    return true;
}

bool EmbeddedGpuTransport::receiveReplyPacket(int socket, quint64 frameId, bool *accepted)
{
    if (!accepted) return false;
    WirePacket reply = {};
    int unexpected[maxPlanes] = {-1, -1, -1, -1};
    int unexpectedCount = 0;
    if (!receivePacket(socket, &reply, unexpected, &unexpectedCount)) return false;
    for (int i = 0; i < unexpectedCount; ++i) ::close(unexpected[i]);
    if (unexpectedCount || reply.magic != wireMagic || reply.version != wireVersion ||
        reply.type != ResultPacket || reply.id != frameId || (reply.result != 0 && reply.result != 1))
        return false;
    *accepted = reply.result == 1;
    return true;
}

bool EmbeddedGpuTransport::sendReplyPacket(int socket, quint64 frameId, bool accepted)
{
    if (socket < 0 || !frameId) return false;
    WirePacket packet = {};
    packet.magic = wireMagic;
    packet.version = wireVersion;
    packet.type = ResultPacket;
    packet.id = frameId;
    packet.result = accepted ? 1 : 0;
    return sendPacket(socket, packet, nullptr, 0);
}

int EmbeddedGpuTransport::duplicatePeer() const
{
    QMutexLocker lock(&m_mutex);
    return m_peer >= 0 ? fcntl(m_peer, F_DUPFD_CLOEXEC, 0) : -1;
}

bool EmbeddedGpuTransport::sendReply(quint64 frameId, bool accepted)
{
    const int fd = duplicatePeer();
    if (fd < 0) return false;
    const bool sent = sendReplyPacket(fd, frameId, accepted);
    ::close(fd);
    return sent;
}

bool EmbeddedGpuTransport::sendFrame(const AuroraDmaBufFrame &frame, QString *error)
{
    QMutexLocker sendLock(&m_sendMutex);
    const int fd = duplicatePeer();
    if (fd < 0) {
        setError(error, tr("The GPU channel is disconnected."));
        return false;
    }
    if (!sendFramePacket(fd, frame)) {
        ::close(fd);
        setError(error, tr("Could not send valid GPU frame metadata and descriptors."));
        return false;
    }
    if (!waitFd(fd, POLLIN, 1200)) {
        ::close(fd);
        setError(error, tr("Qt did not consume the GPU frame before the deadline."));
        return false;
    }
    bool accepted = false;
    const bool received = receiveReplyPacket(fd, frame.id, &accepted);
    ::close(fd);
    if (!received || !accepted) {
        setError(error, tr("Qt rejected the GPU frame; using CPU presentation."));
        return false;
    }
    return true;
}

bool EmbeddedGpuTransport::canExportDmaBuf(QString *error)
{
    if (error) error->clear();
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();
    const char *extensions = display != EGL_NO_DISPLAY ? eglQueryString(display, EGL_EXTENSIONS) : nullptr;
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT ||
        !hasExtension(extensions, "EGL_MESA_image_dma_buf_export") ||
        !hasExtension(extensions, "EGL_KHR_image_base") ||
        !hasExtension(extensions, "EGL_KHR_gl_texture_2D_image")) {
        setError(error, tr("GPU producer lacks the EGL DMA-BUF export extensions."));
        return false;
    }
    if (!eglGetProcAddress("eglCreateImageKHR") || !eglGetProcAddress("eglDestroyImageKHR") ||
        !eglGetProcAddress("eglExportDMABUFImageQueryMESA") ||
        !eglGetProcAddress("eglExportDMABUFImageMESA")) {
        setError(error, tr("GPU producer lacks an EGL DMA-BUF export entry point."));
        return false;
    }
    return true;
}

QStringList EmbeddedGpuTransport::currentGpuCapabilities()
{
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
        return {QStringLiteral("No current EGL/GLES context.")};
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    QString exportError;
    const bool canExport = canExportDmaBuf(&exportError);
    const bool nativeImport = hasExtension(extensions, "EGL_ANDROID_image_native_buffer") &&
        hasExtension(extensions, "EGL_KHR_image_base") && hasGlExtension("GL_OES_EGL_image") &&
        eglGetProcAddress("eglCreateImageKHR") && eglGetProcAddress("eglDestroyImageKHR") &&
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    const char *hybrisEntries[] = {
        "eglHybrisCreateNativeBuffer", "eglHybrisGetNativeBufferInfo",
        "eglHybrisSerializeNativeBuffer", "eglHybrisCreateRemoteBuffer",
        "eglHybrisReleaseNativeBuffer"
    };
    QStringList missingHybrisEntries;
    for (const char *entry : hybrisEntries) {
        if (!eglGetProcAddress(entry)) missingHybrisEntries.append(QString::fromLatin1(entry));
    }
    QLibrary gralloc(QStringLiteral("libgralloc.so.1"));
    QString grallocStatus = QStringLiteral("unavailable: libgralloc.so.1 not loaded");
    if (gralloc.load()) {
        const bool symbols = gralloc.resolve("hybris_gralloc_initialize") &&
            gralloc.resolve("hybris_gralloc_allocate") && gralloc.resolve("hybris_gralloc_release") &&
            gralloc.resolve("hybris_gralloc_deinitialize");
        grallocStatus = !symbols ? QStringLiteral("disabled: Hybris allocator symbols are incomplete")
            : !nativeImport ? QStringLiteral("disabled: no native-buffer EGL/GLES import support")
            : QStringLiteral("disabled: native-buffer ABI and peer import are not verified");
    }
    return {
        QStringLiteral("GLES: %1 (%2)").arg(QString::fromLatin1(renderer), QString::fromLatin1(version)),
        QStringLiteral("DMA-BUF texture export: %1").arg(canExport ? QStringLiteral("advertised") : exportError),
        QStringLiteral("DMA-BUF import: %1; modifiers: %2")
            .arg(hasExtension(extensions, "EGL_EXT_image_dma_buf_import") ? QStringLiteral("advertised") : QStringLiteral("absent"),
                 hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ? QStringLiteral("advertised") : QStringLiteral("absent")),
        QStringLiteral("Native-buffer EGL/GLES import: %1").arg(nativeImport ? QStringLiteral("advertised") : QStringLiteral("absent or incomplete")),
        QStringLiteral("Hybris native-buffer extension: %1; missing entry points: %2")
            .arg(hasExtension(extensions, "EGL_HYBRIS_native_buffer2") ? QStringLiteral("advertised") : QStringLiteral("absent"),
                 missingHybrisEntries.isEmpty() ? QStringLiteral("none") : missingHybrisEntries.join(QStringLiteral(", "))),
        QStringLiteral("Gralloc: %1").arg(grallocStatus),
        QStringLiteral("GBM test: %1").arg(probeGbmRenderTarget(64, 64)),
        QStringLiteral("This probe does not test peer import or screen output.")
    };
}

bool aurora_copy_gpu_texture(GLuint source, GLuint target, int width, int height, QString *error)
{
    if (!source || !target || source == target || width <= 0 || height <= 0) {
        setError(error, QStringLiteral("GPU copy has invalid texture or dimensions"));
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        if (glGetError() == GL_NO_ERROR) break;
        if (i == 15) {
            setError(error, QStringLiteral("GPU context has persistent errors before the copy"));
            return false;
        }
    }
    GLint previous = 0, previousTexture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    bool complete = fbo && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
        complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            glBindTexture(GL_TEXTURE_2D, target);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
            glFinish();
        }
    }
    const GLenum glError = glGetError();
    glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (!complete || glError != GL_NO_ERROR) {
        setError(error, QStringLiteral("GPU texture copy failed (framebuffer complete=%1, GL error 0x%2)")
                 .arg(complete).arg(glError, 0, 16));
        return false;
    }
    return true;
}

static QString processGbmRenderTarget(int width, int height, GLuint source,
    const std::function<bool(const AuroraDmaBufFrame &, QString *)> &consume, bool *success,
    quint64 channel = 0, quint64 generation = 0)
{
    if (success) *success = false;
    if (width <= 0 || height <= 0 || width > maxDimension || height > maxDimension ||
        quint64(width) * quint64(height) > maxFramePixels)
        return QStringLiteral("unavailable: frame dimensions exceed the GPU transport limits");
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT)
        return QStringLiteral("unavailable: no current worker EGL context");
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import"))
        return QStringLiteral("unavailable: worker EGL lacks EGL_EXT_image_dma_buf_import");
    if (!hasExtension(extensions, "EGL_KHR_image_base") || !hasGlExtension("GL_OES_EGL_image"))
        return QStringLiteral("unavailable: worker EGL/GLES lacks image-to-texture support");
#ifndef ACL_HAVE_GBM_HEADER
    Q_UNUSED(source)
    Q_UNUSED(consume)
    Q_UNUSED(channel)
    Q_UNUSED(generation)
    return QStringLiteral("unavailable: build SDK lacks the GBM API header");
#else
    QLibrary gbm(QStringLiteral("libgbm.so.1"));
    if (!gbm.load())
        return QStringLiteral("unavailable: libgbm.so.1 could not be loaded");
#define ACL_RESOLVE_GBM(name) const auto name = reinterpret_cast<decltype(&::name)>(gbm.resolve(#name))
    ACL_RESOLVE_GBM(gbm_create_device);
    ACL_RESOLVE_GBM(gbm_device_destroy);
    ACL_RESOLVE_GBM(gbm_bo_create);
    ACL_RESOLVE_GBM(gbm_bo_destroy);
    ACL_RESOLVE_GBM(gbm_bo_get_format);
    ACL_RESOLVE_GBM(gbm_bo_get_modifier);
    ACL_RESOLVE_GBM(gbm_bo_get_plane_count);
    ACL_RESOLVE_GBM(gbm_bo_get_fd_for_plane);
    ACL_RESOLVE_GBM(gbm_bo_get_stride_for_plane);
    ACL_RESOLVE_GBM(gbm_bo_get_offset);
#undef ACL_RESOLVE_GBM
    if (!gbm_create_device || !gbm_device_destroy || !gbm_bo_create || !gbm_bo_destroy ||
        !gbm_bo_get_format || !gbm_bo_get_modifier || !gbm_bo_get_plane_count ||
        !gbm_bo_get_fd_for_plane || !gbm_bo_get_stride_for_plane || !gbm_bo_get_offset)
        return QStringLiteral("unavailable: libgbm lacks allocation or plane-metadata entry points");

    const QDir dri(QStringLiteral("/dev/dri"));
    const auto nodes = dri.entryList({QStringLiteral("renderD*")}, QDir::System | QDir::NoSymLinks);
    QStringList failures;
    for (const QString &node : nodes) {
        bool renderNumber = false;
        const uint number = node.mid(7).toUInt(&renderNumber);
        if (!renderNumber || number < 128 || node != QStringLiteral("renderD%1").arg(number))
            continue;
        const QByteArray path = QFile::encodeName(dri.filePath(node));
        QFile renderNode;
        const int fd = ::open(path.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            failures.append(QStringLiteral("%1: open denied or unavailable (errno %2)").arg(node).arg(errno));
            continue;
        }
        struct stat info = {};
        if (fstat(fd, &info) != 0 || !S_ISCHR(info.st_mode) ||
            !renderNode.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            failures.append(QStringLiteral("%1: not a usable character device").arg(node));
            continue;
        }
        const std::unique_ptr<gbm_device, decltype(gbm_device_destroy)> device(gbm_create_device(fd), gbm_device_destroy);
        if (!device) {
            failures.append(QStringLiteral("%1: GBM could not initialize the device").arg(node));
            continue;
        }
        const uint32_t formats[] = {GBM_FORMAT_ARGB8888, GBM_FORMAT_XRGB8888};
        const uint32_t flags[] = {GBM_BO_USE_RENDERING, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR};
        QString lastError;
        for (const uint32_t format : formats) {
            if (source && format != GBM_FORMAT_ARGB8888) continue;
            for (const uint32_t use : flags) {
                AuroraDmaBufFrame frame;
                frame.width = width;
                frame.height = height;
                const std::unique_ptr<gbm_bo, decltype(gbm_bo_destroy)> buffer(
                    gbm_bo_create(device.get(), frame.width, frame.height, format, use), gbm_bo_destroy);
                if (!buffer) continue;
                frame.fourcc = int(gbm_bo_get_format(buffer.get()));
                frame.modifier = gbm_bo_get_modifier(buffer.get());
                frame.planeCount = gbm_bo_get_plane_count(buffer.get());
                bool valid = frame.planeCount > 0 && frame.planeCount <= maxPlanes;
                for (int plane = 0; valid && plane < frame.planeCount; ++plane) {
                    const auto stride = gbm_bo_get_stride_for_plane(buffer.get(), plane);
                    const auto offset = gbm_bo_get_offset(buffer.get(), plane);
                    if (!stride || stride > std::numeric_limits<int>::max() ||
                        offset > std::numeric_limits<int>::max()) {
                        valid = false;
                        break;
                    }
                    frame.strides[plane] = int(stride);
                    frame.offsets[plane] = int(offset);
                    frame.fds[plane] = gbm_bo_get_fd_for_plane(buffer.get(), plane);
                    valid = frame.fds[plane] >= 0 &&
                        fcntl(frame.fds[plane], F_SETFD, FD_CLOEXEC) == 0;
                }
                if (!valid) {
                    lastError = QStringLiteral("allocation produced unusable plane metadata or FDs");
                    continue;
                }
                const GLuint texture = aurora_import_dma_buf_texture(frame, display, &lastError);
                if (!texture) continue;
                bool rendered = false;
                if (source) {
                    rendered = aurora_copy_gpu_texture(source, texture, width, height, &lastError);
                } else {
                    GLint previous = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
                    GLuint fbo = 0;
                    glGenFramebuffers(1, &fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                    rendered = fbo && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                    if (rendered) {
                        glClear(GL_COLOR_BUFFER_BIT);
                        glFinish();
                    }
                    const GLenum glError = glGetError();
                    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previous));
                    if (fbo) glDeleteFramebuffers(1, &fbo);
                    rendered = rendered && glError == GL_NO_ERROR;
                    if (!rendered)
                        lastError = QStringLiteral("import succeeded but GLES rendering failed (GL error 0x%1)")
                            .arg(glError, 0, 16);
                }
                glDeleteTextures(1, &texture);
                if (rendered) {
                    const QString producer = QStringLiteral("%1: allocated, imported and rendered a %2x%3 test buffer "
                                                            "(fourcc 0x%4, modifier 0x%5, %6 plane(s))")
                        .arg(node).arg(width).arg(height).arg(frame.fourcc, 0, 16)
                        .arg(frame.modifier, 0, 16).arg(frame.planeCount);
                    if (!consume) {
                        if (success) *success = true;
                        return producer + QStringLiteral("; parent import and presentation not tested");
                    }
                    frame.id = nextFrameId();
                    frame.channel = channel;
                    frame.generation = generation;
                    frame.probeOnly = !source;
                    QString parentError;
                    if (!consume(frame, &parentError))
                        return producer + QStringLiteral("; parent import/ACK failed: ") + parentError;
                    if (success) *success = true;
                    return producer + (source ? QStringLiteral("; live frame acknowledged")
                                             : QStringLiteral("; parent Qt import and GPU snapshot acknowledged; not displayed"));
                }
            }
        }
        if (lastError.isEmpty())
            lastError = QStringLiteral("GBM could not allocate a %1x%2 render target for either tested format")
                .arg(width).arg(height);
        failures.append(QStringLiteral("%1: %2").arg(node, lastError));
    }
    return failures.isEmpty() ? QStringLiteral("unavailable: no visible DRM render node")
                              : QStringLiteral("unavailable: %1").arg(failures.join(QStringLiteral("; ")));
#endif
}

QString EmbeddedGpuTransport::probeGbmRenderTarget(int width, int height,
    const std::function<bool(const AuroraDmaBufFrame &, QString *)> &verifyParent, bool *available)
{
    return processGbmRenderTarget(width, height, 0, verifyParent, available);
}

bool EmbeddedGpuTransport::presentGbmTexture(GLuint texture, int width, int height, QString *error,
                                             quint64 channel, quint64 generation)
{
    if (!texture || bool(channel) != bool(generation)) {
        setError(error, tr("The GPU source texture is invalid."));
        return false;
    }
    bool success = false;
    const QString result = processGbmRenderTarget(width, height, texture,
        [this](const AuroraDmaBufFrame &frame, QString *reason) {
            return sendFrame(frame, reason);
        }, &success, channel, generation);
    if (!success) setError(error, result);
    return success;
}

bool EmbeddedGpuTransport::presentTexture(GLuint texture, int width, int height, QString *error,
                                          quint64 channel, quint64 generation)
{
    if (!texture || width <= 0 || height <= 0 || width > maxDimension || height > maxDimension ||
        quint64(width) * quint64(height) > maxFramePixels || bool(channel) != bool(generation)) {
        setError(error, tr("The GPU texture dimensions are invalid."));
        return false;
    }
    if (!canExportDmaBuf(error)) return false;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    CreateImage createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
    DestroyImage destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
    ExportQuery queryExport = reinterpret_cast<ExportQuery>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
    ExportImage exportImage = reinterpret_cast<ExportImage>(eglGetProcAddress("eglExportDMABUFImageMESA"));
    if (!createImage || !destroyImage || !queryExport || !exportImage) {
        setError(error, tr("GPU producer lacks an EGL DMA-BUF export entry point."));
        return false;
    }
    const EGLint imageAttrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = createImage(display, context, EGL_GL_TEXTURE_2D_KHR,
                                    reinterpret_cast<EGLClientBuffer>(quintptr(texture)), imageAttrs);
    if (image == EGL_NO_IMAGE_KHR) {
        setError(error, tr("Could not create an EGLImage for the GPU texture."));
        return false;
    }
    EGLint fourcc = 0, planeCount = 0;
    EGLuint64KHR modifier = EGLuint64KHR(invalidModifier);
    bool ok = queryExport(display, image, &fourcc, &planeCount, &modifier) &&
              fourcc > 0 && planeCount > 0 && planeCount <= maxPlanes;
    EGLint fds[maxPlanes] = {-1, -1, -1, -1};
    EGLint strides[maxPlanes] = {};
    EGLint offsets[maxPlanes] = {};
    if (ok) ok = exportImage(display, image, fds, strides, offsets);
    AuroraDmaBufFrame exported;
    exported.id = nextFrameId();
    exported.channel = channel;
    exported.generation = generation;
    exported.width = width;
    exported.height = height;
    exported.fourcc = fourcc;
    exported.planeCount = planeCount;
    exported.modifier = quint64(modifier);
    for (int i = 0; ok && i < planeCount; ++i) {
        if (fds[i] < 0 || strides[i] <= 0 || offsets[i] < 0) ok = false;
        exported.fds[i] = fds[i];
        exported.strides[i] = strides[i];
        exported.offsets[i] = offsets[i];
    }
    if (!ok) {
        for (int i = 0; i < maxPlanes; ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
            exported.fds[i] = -1;
        }
        destroyImage(display, image);
        setError(error, tr("EGL could not export valid DMA-BUF plane metadata."));
        return false;
    }
    const bool sent = sendFrame(exported, error);
    exported.close();
    destroyImage(display, image);
    return sent;
}

void EmbeddedGpuTransport::fail(const QString &reason)
{
    closePeer();
    emit failed(reason);
}

GLuint aurora_import_dma_buf_texture(const AuroraDmaBufFrame &frame, EGLDisplay display, QString *error)
{
    if (display == EGL_NO_DISPLAY || eglGetCurrentDisplay() != display ||
        eglGetCurrentContext() == EGL_NO_CONTEXT || frame.width <= 0 || frame.height <= 0 ||
        frame.planeCount <= 0 || frame.planeCount > maxPlanes || frame.fourcc <= 0) {
        setError(error, QStringLiteral("Qt has no compatible current EGL context for DMA-BUF import."));
        return 0;
    }
    if (!modifierSupported(display, frame.fourcc, frame.modifier, error))
        return 0;
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!hasExtension(extensions, "EGL_KHR_image_base") || !hasGlExtension("GL_OES_EGL_image")) {
        setError(error, QStringLiteral("Qt lacks EGL image import extensions."));
        return 0;
    }
    CreateImage createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
    DestroyImage destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
    ImageTargetTexture imageTarget = reinterpret_cast<ImageTargetTexture>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !destroyImage || !imageTarget) {
        setError(error, QStringLiteral("Qt lacks an EGL DMA-BUF import entry point."));
        return 0;
    }
    EGLint attrs[64] = {EGL_WIDTH, frame.width, EGL_HEIGHT, frame.height,
                        EGL_LINUX_DRM_FOURCC_EXT, frame.fourcc};
    int n = 6;
    const EGLint fdAttrs[maxPlanes] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                       EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    const EGLint offsetAttrs[maxPlanes] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                           EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    const EGLint pitchAttrs[maxPlanes] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                          EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    const EGLint modifierLoAttrs[maxPlanes] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    const EGLint modifierHiAttrs[maxPlanes] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (int i = 0; i < frame.planeCount; ++i) {
        if (frame.fds[i] < 0 || frame.strides[i] <= 0 || frame.offsets[i] < 0) {
            setError(error, QStringLiteral("DMA-BUF plane metadata is invalid."));
            return 0;
        }
        attrs[n++] = fdAttrs[i]; attrs[n++] = frame.fds[i];
        attrs[n++] = offsetAttrs[i]; attrs[n++] = frame.offsets[i];
        attrs[n++] = pitchAttrs[i]; attrs[n++] = frame.strides[i];
        if (frame.modifier != invalidModifier) {
            attrs[n++] = modifierLoAttrs[i]; attrs[n++] = EGLint(frame.modifier & 0xffffffffu);
            attrs[n++] = modifierHiAttrs[i]; attrs[n++] = EGLint(frame.modifier >> 32);
        }
    }
    attrs[n++] = EGL_NONE;
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                    nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        setError(error, QStringLiteral("eglCreateImageKHR rejected the exported DMA-BUF metadata."));
        return 0;
    }
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    imageTarget(GL_TEXTURE_2D, image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum glError = glGetError();
    glBindTexture(GL_TEXTURE_2D, GLuint(previous));
    destroyImage(display, image);
    if (!texture || glError != GL_NO_ERROR) {
        if (texture) glDeleteTextures(1, &texture);
        setError(error, QStringLiteral("GLES could not bind the imported EGLImage."));
        return 0;
    }
    return texture;
}
