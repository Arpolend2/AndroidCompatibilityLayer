#include "../embedded_gpu_transport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
typedef EGLImageKHR (*CreateImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*DestroyImage)(EGLDisplay, EGLImageKHR);
typedef EGLBoolean (*ExportQuery)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLuint64KHR *);
typedef EGLBoolean (*ExportImage)(EGLDisplay, EGLImageKHR, EGLint *, EGLint *, EGLint *);
typedef EGLBoolean (*QueryFormats)(EGLDisplay, EGLint, EGLint *, EGLint *);
typedef EGLBoolean (*QueryModifiers)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *);

bool hasExtension(const char *list, const char *name)
{
    if (!list) return false;
    const size_t size = std::strlen(name);
    const char *p = list;
    while ((p = std::strstr(p, name)) != nullptr) {
        if ((p == list || p[-1] == ' ') && (p[size] == ' ' || p[size] == '\0')) return true;
        p += size;
    }
    return false;
}

bool hasModifierIntersection(EGLDisplay display, EGLint format, EGLuint64KHR modifier)
{
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    QueryFormats formats = reinterpret_cast<QueryFormats>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    if (!formats) return false;
    EGLint count = 0;
    if (!formats(display, 0, nullptr, &count) || count <= 0 || count > 4096) return false;
    std::vector<EGLint> supportedFormats(static_cast<size_t>(count));
    EGLint actual = 0;
    if (!formats(display, count, supportedFormats.data(), &actual) || actual < 0 || actual > count ||
        std::find(supportedFormats.begin(), supportedFormats.begin() + actual, format) ==
            supportedFormats.begin() + actual)
        return false;
    if (modifier == EGLuint64KHR(0x00ffffffffffffffULL))
        return true;
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers")) return false;
    QueryModifiers modifiers = reinterpret_cast<QueryModifiers>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!modifiers || !modifiers(display, format, 0, nullptr, nullptr, &count) || count <= 0 || count > 4096)
        return false;
    std::vector<EGLuint64KHR> supportedModifiers(static_cast<size_t>(count));
    std::vector<EGLBoolean> externalOnly(static_cast<size_t>(count));
    actual = 0;
    if (!modifiers(display, format, count, supportedModifiers.data(), externalOnly.data(), &actual) ||
        actual < 0 || actual > count)
        return false;
    for (EGLint i = 0; i < actual; ++i)
        if (supportedModifiers[size_t(i)] == modifier && !externalOnly[size_t(i)]) return true;
    return false;
}

int testLiveGbm(EGLDisplay display, EGLConfig config)
{
    bool available = false;
    const QString probe = EmbeddedGpuTransport::probeGbmRenderTarget(64, 64, {}, &available);
    if (!available) {
        qInfo().noquote() << "SKIP: live GBM test:" << probe;
        return 77;
    }
    QTemporaryDir directory;
    EmbeddedGpuTransport server, client;
    QString error;
    const QString endpoint = directory.path() + QStringLiteral("/gpu");
    if (!directory.isValid() || !server.listen(endpoint, getpid(), &error) ||
        !client.connectTo(endpoint, getpid(), &error)) {
        qWarning().noquote() << "GPU channel setup failed:" << error;
        return 1;
    }
    QElapsedTimer timer;
    timer.start();
    while (!server.connected() && timer.elapsed() < 1000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    if (!server.connected()) return 1;
    int received = 0;
    bool verified = true;
    server.setFrameHandler([&](AuroraDmaBufFrame &&frame) {
        ++received;
        const int expectedWidth = received == 2 ? 80 : 64;
        verified = verified && !frame.probeOnly && frame.width == expectedWidth && frame.height == 64;
        QString importError;
        const GLuint imported = aurora_import_dma_buf_texture(frame, display, &importError);
        GLuint fbo = 0;
        GLubyte actual[80 * 64 * 4] = {};
        if (imported) {
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imported, 0);
            verified = verified && fbo && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glReadPixels(0, 0, expectedWidth, 64, GL_RGBA, GL_UNSIGNED_BYTE, actual);
            verified = verified && glGetError() == GL_NO_ERROR;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < expectedWidth; ++x) {
                    const int at = (y * expectedWidth + x) * 4;
                    verified = verified && actual[at] == GLubyte(17 + x * 3) &&
                        actual[at + 1] == GLubyte(23 + y * 3) && actual[at + 2] == 31 + received * 10 &&
                        actual[at + 3] == 255;
                }
        } else {
            qWarning().noquote() << "GBM parent import failed:" << importError;
            verified = false;
        }
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (imported) glDeleteTextures(1, &imported);
        if (!server.sendReply(frame.id, verified && received < 3)) verified = false;
    });
    std::atomic<bool> done(false);
    bool produced = false;
    QString producerError;
    std::thread producer([&]() {
        const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint surfaceAttrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
        EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
        if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
            eglMakeCurrent(display, surface, surface, context)) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            produced = texture != 0;
            for (int i = 1; produced && i <= 3; ++i) {
                const int width = i == 2 ? 80 : 64;
                GLubyte pixels[80 * 64 * 4] = {};
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < width; ++x) {
                        const int at = (y * width + x) * 4;
                        pixels[at] = GLubyte(17 + x * 3);
                        pixels[at + 1] = GLubyte(23 + y * 3);
                        pixels[at + 2] = GLubyte(31 + i * 10);
                        pixels[at + 3] = 255;
                    }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                producerError.clear();
                const bool accepted = client.presentGbmTexture(texture, width, 64, &producerError);
                produced = (i < 3) ? accepted : (!accepted && !producerError.isEmpty());
            }
            if (texture) glDeleteTextures(1, &texture);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        done.store(true);
    });
    timer.restart();
    while (!done.load() && timer.elapsed() < 6000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    producer.join();
    if (!produced || !verified || received != 3) {
        qWarning().noquote() << "Live GBM copy/import/ACK failed:" << producerError << "frames:" << received;
        return 1;
    }
    qInfo("Live GBM frames, resize and rejection verified");
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool gbmProbe = app.arguments().contains(QStringLiteral("--gbm-probe"));
    const bool copyTest = app.arguments().contains(QStringLiteral("--copy-test"));
    const bool liveGbm = app.arguments().contains(QStringLiteral("--gbm-live"));
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        qInfo("SKIP: no EGL display for DMA-BUF import test");
        return 77;
    }
    const char *eglExtensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!gbmProbe && !copyTest && !liveGbm && (!hasExtension(eglExtensions, "EGL_EXT_image_dma_buf_import") ||
        !hasExtension(eglExtensions, "EGL_MESA_image_dma_buf_export") ||
        !hasExtension(eglExtensions, "EGL_KHR_image_base") ||
        !hasExtension(eglExtensions, "EGL_KHR_gl_texture_2D_image"))) {
        eglTerminate(display);
        qInfo("SKIP: EGL/GLES DMA-BUF export/import extensions are unavailable");
        return 77;
    }

    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &configCount) || configCount != 1) {
        eglTerminate(display);
        qInfo("SKIP: no EGL pbuffer config for DMA-BUF import test");
        return 77;
    }
    const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    const EGLint surfaceAttrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        qInfo("SKIP: EGL could not create an ES2 pbuffer context");
        return 77;
    }
    if (gbmProbe) {
        const QString result = EmbeddedGpuTransport::probeGbmRenderTarget(64, 64);
        qInfo().noquote() << "Embedded GPU GBM probe:" << result;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return result.startsWith(QLatin1String("unavailable:")) ? 77 : 0;
    }
    if (liveGbm) {
        const int result = testLiveGbm(display, config);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return result;
    }
    if (copyTest) {
        const GLubyte pixels[] = {
            19, 53, 91, 255, 101, 42, 77, 128,
            5, 151, 37, 255, 239, 7, 23, 64
        };
        GLuint textures[3] = {};
        GLuint sentinel = 0, targetFbo = 0;
        glGenTextures(3, textures);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &sentinel);
        glBindFramebuffer(GL_FRAMEBUFFER, sentinel);
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        QString error;
        bool ok = aurora_copy_gpu_texture(textures[0], textures[1], 2, 2, &error);
        GLint framebuffer = 0, texture = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        ok = ok && GLuint(framebuffer) == sentinel && GLuint(texture) == textures[2];
        GLubyte actual[sizeof(pixels)] = {};
        glGenFramebuffers(1, &targetFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ok = false;
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        ok = ok && glGetError() == GL_NO_ERROR && std::memcmp(actual, pixels, sizeof(pixels)) == 0;
        ok = ok && !aurora_copy_gpu_texture(textures[0], textures[0], 2, 2, &error) && !error.isEmpty();
        error.clear();
        ok = ok && !aurora_copy_gpu_texture(textures[0], textures[2], 2, 2, &error) && !error.isEmpty();
        glDeleteFramebuffers(1, &targetFbo);
        glDeleteFramebuffers(1, &sentinel);
        glDeleteTextures(3, textures);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        if (!ok) {
            qWarning().noquote() << "GPU texture copy test failed:" << error;
            return 1;
        }
        qInfo("GPU texture copy preserved pixels, alpha, orientation and GL bindings");
        return 0;
    }
    if (!hasExtension(reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)), "GL_OES_EGL_image")) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        qInfo("SKIP: GLES lacks GL_OES_EGL_image");
        return 77;
    }

    const GLubyte sourcePixel[] = {0x13, 0x57, 0x9b, 0xff};
    GLuint source = 0;
    glGenTextures(1, &source);
    glBindTexture(GL_TEXTURE_2D, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint sourceFbo = 0;
    glGenFramebuffers(1, &sourceFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
    if (!source || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &sourceFbo);
        glDeleteTextures(1, &source);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        qInfo("SKIP: EGL driver cannot render to an exportable texture");
        return 77;
    }
    glClearColor(sourcePixel[0] / 255.0f, sourcePixel[1] / 255.0f,
                 sourcePixel[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    CreateImage createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
    DestroyImage destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
    ExportQuery queryExport = reinterpret_cast<ExportQuery>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
    ExportImage exportImage = reinterpret_cast<ExportImage>(eglGetProcAddress("eglExportDMABUFImageMESA"));
    const EGLint imageAttrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = createImage && destroyImage && queryExport && exportImage
        ? createImage(display, context, EGL_GL_TEXTURE_2D_KHR,
        reinterpret_cast<EGLClientBuffer>(quintptr(source)), imageAttrs) : EGL_NO_IMAGE_KHR;
    EGLint fourcc = 0, planeCount = 0;
    EGLuint64KHR modifier = EGLuint64KHR(0x00ffffffffffffffULL);
    bool exported = image != EGL_NO_IMAGE_KHR &&
                    queryExport(display, image, &fourcc, &planeCount, &modifier) &&
                    fourcc > 0 && planeCount > 0 && planeCount <= 4;
    AuroraDmaBufFrame frame;
    if (exported) {
        frame.id = 1;
        frame.width = 4;
        frame.height = 4;
        frame.fourcc = fourcc;
        frame.planeCount = planeCount;
        frame.modifier = quint64(modifier);
        EGLint fds[4] = {-1, -1, -1, -1};
        EGLint strides[4] = {};
        EGLint offsets[4] = {};
        exported = exportImage(display, image, fds, strides, offsets);
        for (int i = 0; i < planeCount; ++i) {
            frame.fds[i] = fds[i];
            frame.strides[i] = strides[i];
            frame.offsets[i] = offsets[i];
            if (fds[i] < 0 || strides[i] <= 0 || offsets[i] < 0) exported = false;
        }
    }
    if (!exported || !hasModifierIntersection(display, fourcc, modifier)) {
        frame.close();
        if (image != EGL_NO_IMAGE_KHR && destroyImage) destroyImage(display, image);
        glDeleteFramebuffers(1, &sourceFbo);
        glDeleteTextures(1, &source);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        qInfo("SKIP: producer export and consumer import have no supported format/modifier intersection");
        return 77;
    }

    int sockets[2] = {-1, -1};
    bool success = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0;
    QString error;
    int acknowledgedFrames = 0;
    for (quint64 frameId = 1; success && frameId <= 3; ++frameId) {
        const GLubyte framePixel[4] = {
            GLubyte(sourcePixel[0] + frameId * 7),
            GLubyte(sourcePixel[1] + frameId * 9),
            GLubyte(sourcePixel[2] + frameId * 11),
            0xff
        };
        glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo);
        glClearColor(framePixel[0] / 255.0f, framePixel[1] / 255.0f,
                     framePixel[2] / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        frame.id = frameId;
        if (!EmbeddedGpuTransport::sendFramePacket(sockets[0], frame)) {
            success = false;
            break;
        }
        AuroraDmaBufFrame received;
        if (!EmbeddedGpuTransport::receiveFramePacket(sockets[1], &received)) {
            success = false;
            break;
        }
        const GLuint imported = aurora_import_dma_buf_texture(received, display, &error);
        GLuint importedFbo = 0;
        GLubyte pixel[4] = {};
        bool frameVerified = false;
        if (imported) {
            glGenFramebuffers(1, &importedFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, importedFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imported, 0);
            frameVerified = importedFbo &&
                glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (frameVerified) {
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                frameVerified = glGetError() == GL_NO_ERROR &&
                    pixel[0] == framePixel[0] && pixel[1] == framePixel[1] &&
                    pixel[2] == framePixel[2] && pixel[3] == framePixel[3];
            }
        }
        if (importedFbo) glDeleteFramebuffers(1, &importedFbo);
        if (imported) glDeleteTextures(1, &imported);
        const bool replySent = EmbeddedGpuTransport::sendReplyPacket(sockets[1], received.id, frameVerified);
        bool accepted = false;
        const bool replyReceived =
            EmbeddedGpuTransport::receiveReplyPacket(sockets[0], frameId, &accepted);
        success = frameVerified && replySent && replyReceived && accepted;
        if (success) ++acknowledgedFrames;
    }
    if (sockets[0] >= 0) close(sockets[0]);
    if (sockets[1] >= 0) close(sockets[1]);
    success = success && acknowledgedFrames == 3;
    frame.close();
    destroyImage(display, image);
    glDeleteFramebuffers(1, &sourceFbo);
    glDeleteTextures(1, &source);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    if (!success) {
        qWarning().noquote() << "DMA-BUF multi-frame import/ACK failed:" << error;
        return 1;
    }
    qInfo("Three DMA-BUF frames imported, pixel-verified, and acknowledged");
    return 0;
}
