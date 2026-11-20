#include "embedded_gpu_transport.h"

#include <QAtomicInteger>
#include <QFile>
#include <QMutexLocker>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const quint32 wireMagic = 0x41434c47;
const quint16 wireVersion = 2;
const int maxPlanes = 4;
const int maxNativeInts = 64;
const int maxDimension = 4096;
const quint64 maxFramePixels = (32 * 1024 * 1024) / 4;
const quint64 invalidModifier = ~quint64(0);

enum WireType {
    FramePacket = 1,
    ResultPacket = 2
};

#pragma pack(push, 1)
struct WirePacket {
    quint32 magic;
    quint16 version;
    quint16 type;
    quint64 id;
    qint32 width;
    qint32 height;
    qint32 fourcc;
    quint32 planeCount;
    quint64 modifier;
    qint32 strides[maxPlanes];
    qint32 offsets[maxPlanes];
    qint32 result;
    quint32 nativeBuffer;
    qint32 nativeFormat;
    qint32 nativeStride;
    quint32 nativeIntCount;
    qint32 nativeInts[maxNativeInts];
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
        packet.type != FramePacket || !packet.id || packet.width <= 0 ||
        packet.height <= 0 || packet.width > maxDimension || packet.height > maxDimension ||
        quint64(packet.width) * quint64(packet.height) > maxFramePixels ||
        !packet.planeCount || packet.planeCount > maxPlanes ||
        fdCount != int(packet.planeCount) || packet.nativeBuffer > 1)
        return false;
    if (packet.nativeBuffer) {
        if (packet.nativeFormat <= 0 || packet.nativeStride < packet.width ||
            packet.nativeIntCount > maxNativeInts)
            return false;
    } else if (packet.fourcc <= 0 || packet.nativeIntCount) {
        return false;
    }
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

/* ---------------------------------------------------------- Hybris gralloc */

struct NativeBase {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(NativeBase *);
    void (*decRef)(NativeBase *);
};

struct NativeWindowBuffer {
    NativeBase common;
    int width;
    int height;
    int stride;
    int format;
    int usageDeprecated;
    size_t layerCount;
    void *reserved;
    const void *handle;
    unsigned long long usage;
    void *reservedProc[7];
};

void nativeBufferNoopRef(NativeBase *) {}

struct GrallocApi {
    void *library = nullptr;
    int (*initialize)(int) = nullptr;
    int (*allocate)(unsigned int, unsigned int, int, unsigned int, void **, unsigned int *) = nullptr;
    int (*release)(void *, int) = nullptr;
    void (*deinitialize)() = nullptr;
    bool attempted = false;
    QString failure;
};

GrallocApi *grallocApi()
{
    // Process-lifetime allocator: do not deinitialize it while worker textures exist.
    static GrallocApi *api = new GrallocApi;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    if (api->attempted) return api;
    api->attempted = true;
    api->library = dlopen("libgralloc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api->library) api->library = dlopen("/usr/lib/libgralloc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        api->failure = QStringLiteral("cannot load Hybris libgralloc.so.1");
        return api;
    }
    api->initialize = reinterpret_cast<decltype(api->initialize)>(dlsym(api->library, "hybris_gralloc_initialize"));
    api->allocate = reinterpret_cast<decltype(api->allocate)>(dlsym(api->library, "hybris_gralloc_allocate"));
    api->release = reinterpret_cast<decltype(api->release)>(dlsym(api->library, "hybris_gralloc_release"));
    api->deinitialize = reinterpret_cast<decltype(api->deinitialize)>(dlsym(api->library, "hybris_gralloc_deinitialize"));
    if (!api->initialize || !api->allocate || !api->release || !api->deinitialize) {
        api->failure = QStringLiteral("Hybris gralloc entry points are incomplete");
    } else if (api->initialize(0) != 0) {
        api->failure = QStringLiteral("hybris_gralloc_initialize failed");
    }
    return api;
}

struct NativeAllocation {
    GrallocApi *api = nullptr;
    void *handle = nullptr;
    int fdCount = 0;
    int intCount = 0;
    unsigned int stride = 0;

    ~NativeAllocation()
    {
        if (api && handle) api->release(handle, 1);
    }
};

bool allocateNative(int width, int height, NativeAllocation *allocation, QString *error)
{
    GrallocApi *api = grallocApi();
    if (!api->failure.isEmpty()) {
        setError(error, api->failure);
        return false;
    }
    void *handle = nullptr;
    unsigned int stride = 0;
    // HAL_PIXEL_FORMAT_BGRA_8888, HW_RENDER | HW_TEXTURE | SW_READ_OFTEN.
    const int result = api->allocate(unsigned(width), unsigned(height), 5, 0x303, &handle, &stride);
    if (result != 0 || !handle || stride < unsigned(width)) {
        setError(error, QStringLiteral("hybris_gralloc_allocate failed with code %1").arg(result));
        return false;
    }
    const int *words = static_cast<const int *>(handle);
    const int fdCount = words[1];
    const int intCount = words[2];
    if (words[0] != 12 || fdCount <= 0 || fdCount > maxPlanes || intCount < 0 || intCount > maxNativeInts) {
        api->release(handle, 1);
        setError(error, QStringLiteral("Hybris returned an unsupported native_handle layout"));
        return false;
    }
    allocation->api = api;
    allocation->handle = handle;
    allocation->fdCount = fdCount;
    allocation->intCount = intCount;
    allocation->stride = stride;
    return true;
}

NativeWindowBuffer nativeWindowBuffer(const void *handle, int width, int height, int stride)
{
    NativeWindowBuffer native = {};
    native.common.magic = 0x5f626672; // ANDROID_NATIVE_BUFFER_MAGIC
    native.common.version = sizeof(native);
    native.common.incRef = nativeBufferNoopRef;
    native.common.decRef = nativeBufferNoopRef;
    native.width = width;
    native.height = height;
    native.stride = stride;
    native.format = 5;
    native.usageDeprecated = 0x303;
    native.layerCount = 1;
    native.handle = handle;
    native.usage = 0x303;
    return native;
}

GLuint importNativeBufferTexture(const AuroraDmaBufFrame &frame, EGLDisplay display, QString *error)
{
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!hasExtension(extensions, "EGL_ANDROID_image_native_buffer") ||
        !hasExtension(extensions, "EGL_KHR_image_base") || !hasGlExtension("GL_OES_EGL_image")) {
        setError(error, QStringLiteral("Qt lacks EGL_ANDROID_image_native_buffer import support"));
        return 0;
    }
    CreateImage createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
    DestroyImage destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
    ImageTargetTexture imageTarget = reinterpret_cast<ImageTargetTexture>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !destroyImage || !imageTarget) {
        setError(error, QStringLiteral("Qt lacks native-buffer EGLImage entry points"));
        return 0;
    }
    std::vector<int> words(size_t(3 + frame.planeCount + frame.nativeIntCount), 0);
    words[0] = 12;
    words[1] = frame.planeCount;
    words[2] = frame.nativeIntCount;
    for (int i = 0; i < frame.planeCount; ++i) words[size_t(3 + i)] = frame.fds[i];
    for (int i = 0; i < frame.nativeIntCount; ++i)
        words[size_t(3 + frame.planeCount + i)] = frame.nativeInts[i];
    NativeWindowBuffer native = nativeWindowBuffer(words.data(), frame.width, frame.height, frame.nativeStride);
    native.format = frame.nativeFormat;
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, 0x3140, // EGL_NATIVE_BUFFER_ANDROID
                                    &native, nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
        setError(error, QStringLiteral("Qt eglCreateImageKHR rejected the Hybris native buffer (EGL 0x%1)")
                            .arg(eglGetError(), 0, 16));
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
        setError(error, QStringLiteral("GLES could not bind the Hybris native-buffer EGLImage"));
        return 0;
    }
    return texture;
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
    nativeBuffer = other.nativeBuffer;
    nativeFormat = other.nativeFormat;
    nativeStride = other.nativeStride;
    nativeIntCount = other.nativeIntCount;
    std::copy(other.nativeInts, other.nativeInts + maxNativeInts, nativeInts);
    other.id = 0;
    other.planeCount = 0;
    other.nativeBuffer = false;
    other.nativeIntCount = 0;
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
    nativeBuffer = false;
    nativeIntCount = 0;
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
        frame.planeCount <= 0 || frame.planeCount > maxPlanes ||
        (!frame.nativeBuffer && (frame.fourcc <= 0 || frame.nativeIntCount != 0)) ||
        (frame.nativeBuffer && (frame.nativeFormat <= 0 || frame.nativeStride < frame.width ||
                               frame.nativeIntCount < 0 || frame.nativeIntCount > maxNativeInts)))
        return false;
    WirePacket packet = {};
    packet.magic = wireMagic;
    packet.version = wireVersion;
    packet.type = FramePacket;
    packet.id = frame.id;
    packet.width = frame.width;
    packet.height = frame.height;
    packet.fourcc = frame.fourcc;
    packet.planeCount = quint32(frame.planeCount);
    packet.modifier = frame.modifier;
    packet.nativeBuffer = frame.nativeBuffer ? 1 : 0;
    packet.nativeFormat = frame.nativeFormat;
    packet.nativeStride = frame.nativeStride;
    packet.nativeIntCount = quint32(frame.nativeIntCount);
    for (int i = 0; i < frame.nativeIntCount; ++i) packet.nativeInts[i] = frame.nativeInts[i];
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
    incoming.width = packet.width;
    incoming.height = packet.height;
    incoming.fourcc = packet.fourcc;
    incoming.planeCount = int(packet.planeCount);
    incoming.modifier = packet.modifier;
    incoming.nativeBuffer = packet.nativeBuffer != 0;
    incoming.nativeFormat = packet.nativeFormat;
    incoming.nativeStride = packet.nativeStride;
    incoming.nativeIntCount = int(packet.nativeIntCount);
    for (int i = 0; i < incoming.nativeIntCount; ++i) incoming.nativeInts[i] = packet.nativeInts[i];
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

bool EmbeddedGpuTransport::canPresentTexture(QString *error)
{
    QString dmaError;
    if (canExportDmaBuf(&dmaError)) {
        if (error) error->clear();
        return true;
    }
    const EGLDisplay display = eglGetCurrentDisplay();
    const char *extensions = display != EGL_NO_DISPLAY ? eglQueryString(display, EGL_EXTENSIONS) : nullptr;
    if (display != EGL_NO_DISPLAY && eglGetCurrentContext() != EGL_NO_CONTEXT &&
        hasExtension(extensions, "EGL_ANDROID_image_native_buffer") &&
        hasExtension(extensions, "EGL_KHR_image_base") && hasGlExtension("GL_OES_EGL_image") &&
        eglGetProcAddress("eglCreateImageKHR") && eglGetProcAddress("eglDestroyImageKHR") &&
        eglGetProcAddress("glEGLImageTargetTexture2DOES") && eglGetProcAddress("glBlitFramebuffer")) {
        GrallocApi *api = grallocApi();
        if (api->failure.isEmpty()) {
            if (error) error->clear();
            return true;
        }
        setError(error, api->failure);
        return false;
    }
    setError(error, dmaError + QStringLiteral(" Hybris native-buffer presentation is also unavailable."));
    return false;
}

bool presentNativeTexture(EmbeddedGpuTransport *transport, GLuint sourceTexture,
                          int width, int height, QString *error)
{
    typedef void (GL_APIENTRYP BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                                GLbitfield, GLenum);
    const GLenum READ_FRAMEBUFFER_ = 0x8CA8;
    const GLenum DRAW_FRAMEBUFFER_ = 0x8CA9;
    BlitFramebuffer blitFramebuffer = reinterpret_cast<BlitFramebuffer>(eglGetProcAddress("glBlitFramebuffer"));
    CreateImage createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
    DestroyImage destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
    ImageTargetTexture imageTarget = reinterpret_cast<ImageTargetTexture>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!blitFramebuffer || !createImage || !destroyImage || !imageTarget) {
        setError(error, QStringLiteral("Native-buffer GPU blit entry points are unavailable"));
        return false;
    }
    NativeAllocation allocation;
    if (!allocateNative(width, height, &allocation, error)) return false;
    NativeWindowBuffer native = nativeWindowBuffer(allocation.handle, width, height, int(allocation.stride));
    const EGLDisplay display = eglGetCurrentDisplay();
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, 0x3140, &native, nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
        setError(error, QStringLiteral("Worker could not create a native-buffer EGLImage (EGL 0x%1)")
                            .arg(eglGetError(), 0, 16));
        return false;
    }

    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    GLint previousFramebuffer = 0, previousTexture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    GLuint nativeTexture = 0;
    GLuint framebuffers[2] = {0, 0};
    glGenTextures(1, &nativeTexture);
    glBindTexture(GL_TEXTURE_2D, nativeTexture);
    imageTarget(GL_TEXTURE_2D, image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    destroyImage(display, image);
    glGenFramebuffers(2, framebuffers);
    glBindFramebuffer(READ_FRAMEBUFFER_, framebuffers[0]);
    glFramebufferTexture2D(READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);
    glBindFramebuffer(DRAW_FRAMEBUFFER_, framebuffers[1]);
    glFramebufferTexture2D(DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, nativeTexture, 0);
    bool ok = nativeTexture && framebuffers[0] && framebuffers[1] &&
              glCheckFramebufferStatus(READ_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE &&
              glCheckFramebufferStatus(DRAW_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE &&
              glGetError() == GL_NO_ERROR;
    if (ok) {
        blitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glFinish();
        ok = glGetError() == GL_NO_ERROR;
    }
    if (!ok) setError(error, QStringLiteral("GPU blit into the Hybris gralloc framebuffer failed"));

    bool sent = false;
    if (ok) {
        static QAtomicInteger<quint64> nextNativeId(0x100000000ULL);
        AuroraDmaBufFrame frame;
        frame.id = nextNativeId.fetchAndAddRelaxed(1);
        if (!frame.id) frame.id = nextNativeId.fetchAndAddRelaxed(1);
        frame.width = width;
        frame.height = height;
        frame.planeCount = allocation.fdCount;
        frame.nativeBuffer = true;
        frame.nativeFormat = 5;
        frame.nativeStride = int(allocation.stride);
        frame.nativeIntCount = allocation.intCount;
        const int *words = static_cast<const int *>(allocation.handle);
        for (int i = 0; i < allocation.fdCount; ++i) {
            frame.fds[i] = words[3 + i];
            frame.strides[i] = int(allocation.stride * 4);
            frame.offsets[i] = 0;
        }
        for (int i = 0; i < allocation.intCount; ++i)
            frame.nativeInts[i] = words[3 + allocation.fdCount + i];
        sent = transport->sendFrame(frame, error);
        // sendFrame duplicates the descriptors into the socket; the allocation owns the originals.
        frame.fds[0] = frame.fds[1] = frame.fds[2] = frame.fds[3] = -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
    glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
    glDeleteFramebuffers(2, framebuffers);
    if (nativeTexture) glDeleteTextures(1, &nativeTexture);
    return sent;
}

bool EmbeddedGpuTransport::presentTexture(GLuint texture, int width, int height, QString *error)
{
    if (!texture || width <= 0 || height <= 0 || width > maxDimension || height > maxDimension ||
        quint64(width) * quint64(height) > maxFramePixels) {
        setError(error, tr("The GPU texture dimensions are invalid."));
        return false;
    }
    if (!canExportDmaBuf(error)) return presentNativeTexture(this, texture, width, height, error);
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
    static QAtomicInteger<quint64> nextId(1);
    AuroraDmaBufFrame exported;
    exported.id = nextId.fetchAndAddRelaxed(1);
    if (!exported.id) exported.id = nextId.fetchAndAddRelaxed(1);
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
        frame.planeCount <= 0 || frame.planeCount > maxPlanes || (!frame.nativeBuffer && frame.fourcc <= 0)) {
        setError(error, QStringLiteral("Qt has no compatible current EGL context for GPU frame import."));
        return 0;
    }
    if (frame.nativeBuffer) return importNativeBufferTexture(frame, display, error);
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
