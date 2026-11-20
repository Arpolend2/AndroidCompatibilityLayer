#include "gpu_probe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

namespace {

QStringList g_lines;

void say(const QString &text)
{
    g_lines << text;
}

QString errnoText(int e)
{
    return QStringLiteral("%1 (%2)").arg(QString::fromLocal8Bit(std::strerror(e))).arg(e);
}

/* ---------------------------------------------------------------- EGL */

struct EglApi {
    void *library = nullptr;
    void *(*getDisplay)(void *) = nullptr;
    unsigned int (*initialize)(void *, int *, int *) = nullptr;
    int (*terminate)(void *) = nullptr;
    int (*queryError)() = nullptr;
    const char *(*queryString)(void *, int) = nullptr;
    void *(*getProcAddress)(const char *) = nullptr;
    void *(*chooseConfig)(void *, const int *, void **, int, int *) = nullptr;
    void *(*createContext)(void *, void *, void *, const int *) = nullptr;
    int (*makeCurrent)(void *, void *, void *, void *) = nullptr;
    int (*destroyContext)(void *, void *) = nullptr;
    int (*releaseThread)() = nullptr;
};

const int EGL_EXTENSIONS_ = 0x3055;
const int EGL_VENDOR_ = 0x3053;
const int EGL_VERSION_ = 0x3054;
const int EGL_CLIENT_APIS_ = 0x308D;
const int EGL_NO_DISPLAY_ = 0;
const int EGL_PBUFFER_BIT_ = 0x0001;
const int EGL_OPENGL_ES2_BIT_ = 0x0004;
const int EGL_RENDERABLE_TYPE_ = 0x3040;
const int EGL_SURFACE_TYPE_ = 0x3033;
const int EGL_NONE_ = 0x3038;
const int EGL_CONTEXT_CLIENT_VERSION_ = 0x3098;
const int EGL_OPENGL_ES_API_ = 0x30A0;

struct NativeBase {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(NativeBase *);
    void (*decRef)(NativeBase *);
};

/* android_native_buffer_t, including the fields added after Android 4. */
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

/* This is the fallback demonstrated by PenguDroid: Hybris talks to the Android
   allocator for us, so there is no need to reproduce MTK's private handle or
   its binder protocol.  The rendered FBO is checked through the native fd,
   deliberately without glReadPixels. */
void probeHybrisGralloc(void *display, const EglApi &egl, void *gles)
{
    void *library = dlopen("libgralloc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("/usr/lib/libgralloc.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        say(QStringLiteral("Hybris gralloc: cannot load libgralloc.so.1: %1")
                .arg(QString::fromLocal8Bit(dlerror())));
        return;
    }
    using Initialize = int (*)(int);
    using Allocate = int (*)(unsigned int, unsigned int, int, unsigned int, void **, unsigned int *);
    using Release = int (*)(void *, int);
    using Deinitialize = void (*)();
    auto initialize = reinterpret_cast<Initialize>(dlsym(library, "hybris_gralloc_initialize"));
    auto allocate = reinterpret_cast<Allocate>(dlsym(library, "hybris_gralloc_allocate"));
    auto release = reinterpret_cast<Release>(dlsym(library, "hybris_gralloc_release"));
    auto deinitialize = reinterpret_cast<Deinitialize>(dlsym(library, "hybris_gralloc_deinitialize"));
    if (!initialize || !allocate || !release || !deinitialize) {
        say(QStringLiteral("Hybris gralloc: the four hybris_gralloc_* entry points are not all available"));
        dlclose(library);
        return;
    }

    void *handle = nullptr;
    unsigned int stride = 0;
    const int initialized = initialize(0);
    const int result = allocate(64, 64, 5, 0x303, &handle, &stride);
    say(QStringLiteral("Hybris gralloc: initialize=%1 allocate=%2 stride=%3 handle=%4")
            .arg(initialized).arg(result).arg(stride).arg(handle ? QStringLiteral("non-null") : QStringLiteral("null")));
    if (result != 0 || !handle || stride < 64) {
        deinitialize();
        dlclose(library);
        return;
    }
    const int *words = static_cast<const int *>(handle);
    const int fdCount = words[1];
    const int intCount = words[2];
    say(QStringLiteral("Hybris gralloc: native_handle version=%1 fds=%2 ints=%3")
            .arg(words[0]).arg(fdCount).arg(intCount));

    void *mapped = MAP_FAILED;
    const size_t length = size_t(stride) * 64 * 4;
    for (int i = 0; i < fdCount && i < 16; ++i) {
        const int candidate = words[3 + i];
        if (candidate < 0) continue;
        mapped = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, candidate, 0);
        say(QStringLiteral("Hybris gralloc: fd[%1]=%2 mmap(%3 bytes): %4")
                .arg(i).arg(candidate).arg(length)
                .arg(mapped == MAP_FAILED ? errnoText(errno) : QStringLiteral("ok")));
        if (mapped != MAP_FAILED) break;
    }

    using CreateImage = void *(*)(void *, void *, unsigned int, void *, const int *);
    using DestroyImage = int (*)(void *, void *);
    using ImageTarget = void (*)(unsigned int, void *);
    using GenTextures = void (*)(int, unsigned int *);
    using BindTexture = void (*)(unsigned int, unsigned int);
    using TexParameter = void (*)(unsigned int, unsigned int, int);
    using GenFramebuffers = void (*)(int, unsigned int *);
    using BindFramebuffer = void (*)(unsigned int, unsigned int);
    using FramebufferTexture = void (*)(unsigned int, unsigned int, unsigned int, unsigned int, int);
    using CheckFramebuffer = unsigned int (*)(unsigned int);
    using Viewport = void (*)(int, int, int, int);
    using ClearColor = void (*)(float, float, float, float);
    using Clear = void (*)(unsigned int);
    using Finish = void (*)();
    using GetError = unsigned int (*)();
    using DeleteFramebuffers = void (*)(int, const unsigned int *);
    using DeleteTextures = void (*)(int, const unsigned int *);
    auto createImage = reinterpret_cast<CreateImage>(egl.getProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<DestroyImage>(egl.getProcAddress("eglDestroyImageKHR"));
    auto imageTarget = reinterpret_cast<ImageTarget>(egl.getProcAddress("glEGLImageTargetTexture2DOES"));
    auto genTextures = reinterpret_cast<GenTextures>(dlsym(gles, "glGenTextures"));
    auto bindTexture = reinterpret_cast<BindTexture>(dlsym(gles, "glBindTexture"));
    auto texParameter = reinterpret_cast<TexParameter>(dlsym(gles, "glTexParameteri"));
    auto genFramebuffers = reinterpret_cast<GenFramebuffers>(dlsym(gles, "glGenFramebuffers"));
    auto bindFramebuffer = reinterpret_cast<BindFramebuffer>(dlsym(gles, "glBindFramebuffer"));
    auto framebufferTexture = reinterpret_cast<FramebufferTexture>(dlsym(gles, "glFramebufferTexture2D"));
    auto checkFramebuffer = reinterpret_cast<CheckFramebuffer>(dlsym(gles, "glCheckFramebufferStatus"));
    auto viewport = reinterpret_cast<Viewport>(dlsym(gles, "glViewport"));
    auto clearColor = reinterpret_cast<ClearColor>(dlsym(gles, "glClearColor"));
    auto clear = reinterpret_cast<Clear>(dlsym(gles, "glClear"));
    auto finish = reinterpret_cast<Finish>(dlsym(gles, "glFinish"));
    auto getError = reinterpret_cast<GetError>(dlsym(gles, "glGetError"));
    auto deleteFramebuffers = reinterpret_cast<DeleteFramebuffers>(dlsym(gles, "glDeleteFramebuffers"));
    auto deleteTextures = reinterpret_cast<DeleteTextures>(dlsym(gles, "glDeleteTextures"));
    if (!createImage || !destroyImage || !imageTarget || !genTextures || !bindTexture ||
        !genFramebuffers || !bindFramebuffer || !framebufferTexture || !checkFramebuffer ||
        !viewport || !clearColor || !clear || !finish || !getError) {
        say(QStringLiteral("Hybris gralloc: EGLImage/FBO test lacks a required entry point"));
    } else {
        NativeWindowBuffer native = {};
        native.common.magic = 0x5f626672; /* ANDROID_NATIVE_BUFFER_MAGIC, "_bfr" */
        native.common.version = sizeof(native);
        native.common.incRef = nativeBufferNoopRef;
        native.common.decRef = nativeBufferNoopRef;
        native.width = 64;
        native.height = 64;
        native.stride = int(stride);
        native.format = 5; /* HAL_PIXEL_FORMAT_BGRA_8888 */
        native.usageDeprecated = 0x303;
        native.layerCount = 1;
        native.handle = handle;
        native.usage = 0x303;
        void *image = createImage(display, nullptr, 0x3140, &native, nullptr);
        say(QStringLiteral("Hybris gralloc: EGL_NATIVE_BUFFER_ANDROID image: %1 (EGL error 0x%2)")
                .arg(image ? QStringLiteral("created") : QStringLiteral("failed"))
                .arg(egl.queryError(), 0, 16));
        if (image) {
            unsigned int texture = 0;
            unsigned int framebuffer = 0;
            genTextures(1, &texture);
            bindTexture(0x0DE1 /* GL_TEXTURE_2D */, texture);
            imageTarget(0x0DE1, image);
            if (texParameter) {
                texParameter(0x0DE1, 0x2801, 0x2600);
                texParameter(0x0DE1, 0x2800, 0x2600);
            }
            const unsigned int targetError = getError();
            genFramebuffers(1, &framebuffer);
            bindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, framebuffer);
            framebufferTexture(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x0DE1, texture, 0);
            const unsigned int status = checkFramebuffer(0x8D40);
            say(QStringLiteral("Hybris gralloc: texture GL error=0x%1 FBO status=0x%2")
                    .arg(targetError, 0, 16).arg(status, 0, 16));
            if (targetError == 0 && status == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
                while (getError() != 0) {}
                viewport(0, 0, 64, 64);
                clearColor(1.0f, 0.25f, 0.5f, 1.0f);
                clear(0x00004000 /* GL_COLOR_BUFFER_BIT */);
                finish();
                say(QStringLiteral("Hybris gralloc: GPU clear+finish GL error=0x%1").arg(getError(), 0, 16));
                if (mapped != MAP_FAILED) {
                    const unsigned char *pixel = static_cast<const unsigned char *>(mapped) + (32 * stride + 32) * 4;
                    say(QStringLiteral("Hybris gralloc: native-fd pixel B,G,R,A=%1,%2,%3,%4 (no glReadPixels)")
                            .arg(pixel[0]).arg(pixel[1]).arg(pixel[2]).arg(pixel[3]));
                }
            }
            bindFramebuffer(0x8D40, 0);
            if (deleteFramebuffers && framebuffer) deleteFramebuffers(1, &framebuffer);
            bindTexture(0x0DE1, 0);
            if (deleteTextures && texture) deleteTextures(1, &texture);
            destroyImage(display, image);
        }
    }
    if (mapped != MAP_FAILED) munmap(mapped, length);
    release(handle, 1);
    deinitialize();
    dlclose(library);
}

bool loadEgl(EglApi *api)
{
    api->library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!api->library) api->library = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!api->library) {
        say(QStringLiteral("EGL: libEGL cannot be loaded: %1").arg(QString::fromLocal8Bit(dlerror())));
        return false;
    }
    const auto sym = [api](const char *name) { return dlsym(api->library, name); };
    api->getDisplay = reinterpret_cast<decltype(api->getDisplay)>(sym("eglGetDisplay"));
    api->initialize = reinterpret_cast<decltype(api->initialize)>(sym("eglInitialize"));
    api->terminate = reinterpret_cast<decltype(api->terminate)>(sym("eglTerminate"));
    api->queryError = reinterpret_cast<decltype(api->queryError)>(sym("eglGetError"));
    api->queryString = reinterpret_cast<decltype(api->queryString)>(sym("eglQueryString"));
    api->getProcAddress = reinterpret_cast<decltype(api->getProcAddress)>(sym("eglGetProcAddress"));
    api->chooseConfig = reinterpret_cast<decltype(api->chooseConfig)>(sym("eglChooseConfig"));
    api->createContext = reinterpret_cast<decltype(api->createContext)>(sym("eglCreateContext"));
    api->makeCurrent = reinterpret_cast<decltype(api->makeCurrent)>(sym("eglMakeCurrent"));
    api->destroyContext = reinterpret_cast<decltype(api->destroyContext)>(sym("eglDestroyContext"));
    api->releaseThread = reinterpret_cast<decltype(api->releaseThread)>(sym("eglReleaseThread"));
    return api->getDisplay && api->initialize && api->queryString;
}

void probeEgl()
{
    EglApi egl;
    if (!loadEgl(&egl)) return;

    const char *client = nullptr;
    if (egl.queryString) client = egl.queryString(reinterpret_cast<void *>(EGL_NO_DISPLAY_), EGL_EXTENSIONS_);
    say(QStringLiteral("EGL client extensions: %1").arg(client ? QString::fromLatin1(client) : QStringLiteral("(none)")));

    void *display = egl.getDisplay(nullptr);
    if (!display) {
        say(QStringLiteral("EGL: eglGetDisplay(EGL_DEFAULT_DISPLAY) failed: 0x%1").arg(egl.queryError ? egl.queryError() : 0, 0, 16));
        return;
    }
    int major = 0, minor = 0;
    if (!egl.initialize(display, &major, &minor)) {
        say(QStringLiteral("EGL: eglInitialize failed: 0x%1").arg(egl.queryError ? egl.queryError() : 0, 0, 16));
        return;
    }
    const auto q = [&](int what) {
        const char *s = egl.queryString(display, what);
        return QString::fromLatin1(s ? s : "");
    };
    say(QStringLiteral("EGL: %1.%2").arg(major).arg(minor));
    say(QStringLiteral("EGL vendor: %1").arg(q(EGL_VENDOR_)));
    say(QStringLiteral("EGL version: %1").arg(q(EGL_VERSION_)));
    say(QStringLiteral("EGL client APIs: %1").arg(q(EGL_CLIENT_APIS_)));
    const QString extensions = q(EGL_EXTENSIONS_);
    say(QStringLiteral("EGL display extensions: %1").arg(extensions));

    const char *wanted[] = {
        "EGL_ANDROID_image_native_buffer", "EGL_ANDROID_native_fence_sync",
        "EGL_EXT_image_dma_buf_import", "EGL_EXT_image_dma_buf_import_modifiers",
        "EGL_MESA_image_dma_buf_export", "EGL_KHR_image_base", "EGL_KHR_gl_texture_2D_image",
        "EGL_KHR_surfaceless_context", "EGL_ANDROID_framebuffer_target", "EGL_KHR_fence_sync",
    };
    for (const char *name : wanted) {
        const bool present = extensions.split(QLatin1Char(' ')).contains(QString::fromLatin1(name));
        say(QStringLiteral("EGL extension %1: %2").arg(QString::fromLatin1(name),
                                                        present ? QStringLiteral("present") : QStringLiteral("absent")));
    }
    /* The entry points a gralloc transport or a dma-buf one would call. */
    const char *entries[] = {"eglCreateImageKHR", "eglDestroyImageKHR", "eglExportDMABUFImageQueryMESA",
                             "eglExportDMABUFImageMESA", "eglCreateSyncKHR", "eglGetNativeClientBufferANDROID"};
    for (const char *name : entries) {
        say(QStringLiteral("EGL entry %1: %2").arg(QString::fromLatin1(name),
                                                   egl.getProcAddress && egl.getProcAddress(name)
                                                       ? QStringLiteral("present") : QStringLiteral("absent")));
    }

    /* The renderer, which says whether this is the device's own driver or a fallback. */
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!gles) {
        say(QStringLiteral("GLES: libGLESv2 cannot be loaded: %1").arg(QString::fromLocal8Bit(dlerror())));
        return;
    }
    const int configAttrs[] = {EGL_RENDERABLE_TYPE_, EGL_OPENGL_ES2_BIT_, EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_NONE_};
    void *config = nullptr;
    int configCount = 0;
    if (egl.chooseConfig && egl.chooseConfig(display, configAttrs, &config, 1, &configCount) && configCount > 0) {
        void *share = nullptr;
        const int contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION_, 2, EGL_NONE_};
        void *context = egl.createContext ? egl.createContext(display, config, share, contextAttrs) : nullptr;
        if (context && egl.makeCurrent && egl.makeCurrent(display, nullptr, nullptr, context)) {
            using GetString = const unsigned char *(*)(unsigned int);
            auto getString = reinterpret_cast<GetString>(dlsym(gles, "glGetString"));
            if (getString) {
                const unsigned int GL_VENDOR_ = 0x1F00, GL_RENDERER_ = 0x1F01, GL_VERSION_ = 0x1F02;
                say(QStringLiteral("GL vendor: %1").arg(QString::fromLatin1(reinterpret_cast<const char *>(getString(GL_VENDOR_)))));
                say(QStringLiteral("GL renderer: %1").arg(QString::fromLatin1(reinterpret_cast<const char *>(getString(GL_RENDERER_)))));
                say(QStringLiteral("GL version: %1").arg(QString::fromLatin1(reinterpret_cast<const char *>(getString(GL_VERSION_)))));
            }
            probeHybrisGralloc(display, egl, gles);
            if (egl.makeCurrent) egl.makeCurrent(display, nullptr, nullptr, nullptr);
        } else {
            say(QStringLiteral("GLES: no context (surfaceless make-current failed: 0x%1)")
                    .arg(egl.queryError ? egl.queryError() : 0, 0, 16));
        }
        if (context && egl.destroyContext) egl.destroyContext(display, context);
    } else {
        say(QStringLiteral("EGL: no ES2 config found"));
    }
    if (egl.releaseThread) egl.releaseThread();
}

/* ---------------------------------------------------------------- the platform allocator */

/* The ION ABI of the kernels Aurora runs on (uapi/linux/ion.h of 4.x). */
struct IonAllocationData {
    size_t len;
    size_t align;
    unsigned int heap_id_mask;
    unsigned int flags;
    int handle;
};
struct IonFdData {
    int handle;
    int fd;
};
struct IonHeapData {
    char name[16];
    unsigned int type;
    unsigned int heap_id;
    unsigned int reserved0, reserved1, reserved2;
};
struct IonHeapQuery {
    unsigned int cnt;
    unsigned int reserved0;
    unsigned long long heaps;
    unsigned int reserved1[11];
};

const unsigned long ION_IOC_ALLOC_ = _IOWR('I', 0, IonAllocationData);
const unsigned long ION_IOC_MAP_ = _IOWR('I', 2, IonFdData);
const unsigned long ION_IOC_SHARE_ = _IOWR('I', 4, IonFdData);
const unsigned long ION_IOC_HEAP_QUERY_ = _IOWR('I', 8, IonHeapQuery);

void probeIon()
{
    say(QStringLiteral("/dev/dma_heap: %1").arg(QFileInfo(QStringLiteral("/dev/dma_heap")).isDir()
                                                     ? QStringLiteral("present") : QStringLiteral("absent")));
    const int ion = ::open("/dev/ion", O_RDWR);
    if (ion < 0) {
        say(QStringLiteral("/dev/ion: cannot be opened: %1").arg(errnoText(errno)));
        return;
    }
    say(QStringLiteral("/dev/ion: opened read-write"));

    IonHeapData heaps[16];
    IonHeapQuery query;
    std::memset(&query, 0, sizeof(query));
    std::memset(heaps, 0, sizeof(heaps));
    query.cnt = 16;
    query.heaps = reinterpret_cast<unsigned long long>(heaps);
    if (::ioctl(ion, ION_IOC_HEAP_QUERY_, &query) == 0) {
        say(QStringLiteral("/dev/ion: %1 heap(s)").arg(query.cnt));
        for (unsigned int i = 0; i < query.cnt && i < 16; ++i) {
            say(QStringLiteral("  heap %1: id=%2 type=%3 name=\"%4\"")
                    .arg(i).arg(heaps[i].heap_id).arg(heaps[i].type)
                    .arg(QString::fromLocal8Bit(heaps[i].name, int(strnlen(heaps[i].name, 16)))));
        }
    } else {
        say(QStringLiteral("/dev/ion: ION_IOC_HEAP_QUERY failed: %1").arg(errnoText(errno)));
    }

    /* 1 MiB, 4 KiB-aligned, both a mask of each low bit and the usual carveout bits. */
    const unsigned int masks[] = {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5,
                                  0xffffffffu, 0x00000003u, 0x40000000u, 0x10000000u};
    for (unsigned int mask : masks) {
        IonAllocationData request;
        std::memset(&request, 0, sizeof(request));
        request.len = 1024 * 1024;
        request.align = 4096;
        request.heap_id_mask = mask;
        request.handle = -1;
        if (::ioctl(ion, ION_IOC_ALLOC_, &request) != 0 || request.handle < 0) {
            say(QStringLiteral("ION alloc mask 0x%1: failed: %2")
                    .arg(mask, 8, 16, QLatin1Char('0')).arg(errnoText(errno)));
            continue;
        }
        IonFdData fdData;
        std::memset(&fdData, 0, sizeof(fdData));
        fdData.handle = request.handle;
        fdData.fd = -1;
        const bool shared = ::ioctl(ion, ION_IOC_SHARE_, &fdData) == 0 && fdData.fd >= 0;
        if (!shared) {
            const int shareErrno = errno;
            std::memset(&fdData, 0, sizeof(fdData));
            fdData.handle = request.handle;
            fdData.fd = -1;
            if (::ioctl(ion, ION_IOC_MAP_, &fdData) != 0 || fdData.fd < 0) {
                say(QStringLiteral("ION alloc mask 0x%1: allocated, but neither ION_IOC_SHARE (%2) nor ION_IOC_MAP (%3) gave an fd")
                        .arg(mask, 8, 16, QLatin1Char('0')).arg(errnoText(shareErrno)).arg(errnoText(errno)));
                ::close(ion);
                return;
            }
        }
        void *mapped = ::mmap(nullptr, request.len, PROT_READ | PROT_WRITE, MAP_SHARED, fdData.fd, 0);
        if (mapped == MAP_FAILED) {
            say(QStringLiteral("ION alloc mask 0x%1: fd %2, but mmap failed: %3")
                    .arg(mask, 8, 16, QLatin1Char('0')).arg(fdData.fd).arg(errnoText(errno)));
            ::close(fdData.fd);
            ::close(ion);
            return;
        }
        unsigned char *bytes = static_cast<unsigned char *>(mapped);
        for (size_t i = 0; i < request.len; i += 4096) bytes[i] = static_cast<unsigned char>(i / 4096);
        bool ok = true;
        for (size_t i = 0; i < request.len; i += 4096)
            if (bytes[i] != static_cast<unsigned char>(i / 4096)) ok = false;
        say(QStringLiteral("ION alloc mask 0x%1: fd %2, %3 bytes mapped and %4")
                .arg(mask, 8, 16, QLatin1Char('0')).arg(fdData.fd).arg(request.len)
                .arg(ok ? QStringLiteral("writable") : QStringLiteral("NOT writable through the mapping")));
        ::munmap(mapped, request.len);
        ::close(fdData.fd);
        ::close(ion);
        return;
    }
    ::close(ion);
}

/* The libraries the device's own gralloc is inside: Android's, so a glibc program such as
   this one cannot load them -- the runtime loads them with its own linker (core/src/ld)
   and its own bionic. Their presence is what matters here. */
void probeVendorLibraries()
{
    const char *paths[] = {"/vendor/lib/libgpud.so", "/vendor/lib/libgpu_aux.so",
                           "/vendor/lib/libgralloc_extra.so", "/vendor/lib/libion_mtk.so",
                           "/vendor/lib/egl/libEGL_mtk.so", "/vendor/lib/libIMGegl.so",
                           "/system/lib/libEGL.so"};
    for (const char *path : paths) {
        const QFileInfo info(QString::fromLatin1(path));
        if (!info.exists()) {
            say(QStringLiteral("%1: absent").arg(QString::fromLatin1(path)));
            continue;
        }
        void *handle = dlopen(path, RTLD_NOW);
        say(QStringLiteral("%1: %2 bytes, glibc dlopen: %3")
                .arg(QString::fromLatin1(path)).arg(info.size())
                .arg(handle ? QStringLiteral("loaded") : QString::fromLocal8Bit(dlerror())));
        if (handle) dlclose(handle);
    }
}

} // namespace

QString GpuProbe::report()
{
    g_lines.clear();
    say(QStringLiteral("GPU probe: what this device offers for a frame between processes"));
    probeEgl();
    probeIon();
    probeVendorLibraries();

    QString text = g_lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    const QStringList directories = {
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        QStringLiteral("/tmp"),
    };
    for (const QString &directory : directories) {
        if (directory.isEmpty()) continue;
        QFile file(QDir(directory).filePath(QStringLiteral("gpu-probe.txt")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(text.toUtf8());
            file.close();
        }
    }
    return text;
}
