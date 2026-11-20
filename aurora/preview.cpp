#include "preview.h"
#include "google_services_runtime.h"

#include <QCoreApplication>
#include <QAtomicInt>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <cstdlib>

#include "preview_text.h"
#include "preview_frame.h"
#include "preview_keyboard.h"
#include "audio_output.h"
#include "audio_mp3_codec.h"
#include "media_mp3_host.h"
#include "camera_host.h"
#include "bluetooth_host.h"
#include "sensor_host.h"
#include "location_host.h"
#include "picker_host.h"
#include "shared_folders.h"
#include "install_request_host.h"
#include "preview_viewport.h"
#include "preview_wheel.h"
#include "runtime_settings.h"
#include "log_channel.h"
#include "error_log.h"
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include "../core/src/ndk/mg_egl.h"
#include "preview_gpu.h"
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
#include "preview_gpu_canvas.h"
#endif
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
#include "embedded_gpu_transport.h"
#endif

namespace {
void MEGLER_CB coreLog(int level, const char *line, void *user)
{
    const int fd = *static_cast<const int *>(user);
    if (fd >= 0) LogChannel::sendLine(level, line, user);
    ErrorLog::sendCoreLine(level, line);
}
}

PreviewPlayer::PreviewPlayer(const QString &root, const QString &data, const QString &apk, QObject *parent)
    : QObject(parent), m_data(data), m_renderCap(RuntimeSettings::renderCap()),
      m_localeUtf8(RuntimeSettings::locale().toUtf8()), m_theme(RuntimeSettings::theme()),
      m_location(RuntimeSettings::location()), m_rootUtf8(root.toUtf8()),
      m_apkUtf8(apk.toUtf8()),
      m_splitsUtf8(QFile::encodeName(QFileInfo(apk).dir().filePath(QStringLiteral("splits")))),
      m_status(tr("Starting Android app..."))
{
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    m_gpuState = std::make_shared<PreviewGpuState>();
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    if (!m_gpuCanvas.setMetricsEnabled([]() { return ErrorLog::detailedCaptureEnabled(); }))
        qWarning("Could not configure GPU diagnostics.");
#endif
}

void PreviewPlayer::enableStoreWorkerTeardownTrace()
{
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    m_gpuCanvas.enableStoreWorkerTeardownTrace();
#endif
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    m_gpuState->enableStoreWorkerTeardownTrace();
#endif
}

void PreviewPlayer::setStandaloneSettings(const QString &locale, const QString &theme, const QString &location)
{
    m_localeUtf8 = RuntimeSettings::localeForSelection(locale).toUtf8();
    m_theme = theme;
    m_location = location;
}

void PreviewPlayer::setSystemTheme(bool dark, const QColor &accent)
{
    m_systemDark = dark;
    m_systemAccent = accent;
}

QImage PreviewPlayer::frame() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_frame;
}

void PreviewPlayer::setStatus(const QString &status)
{
    m_status = status;
    emit statusChanged();
}

void PreviewPlayer::start()
{
    if (m_closing) return;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    const EGLDisplay qtDisplay = m_cpuPresentationOnly ? EGL_NO_DISPLAY : m_gpuState->qtDisplay();
    if (qtDisplay != EGL_NO_DISPLAY) {
        if (!mg_egl_set_native_display(qtDisplay))
            qWarning("Could not select Qt's EGL display before Android EGL initialization");
    } else if (!m_cpuPresentationOnly && m_surface && m_surface->window() && m_gpuDisplayWaits < 20) {
        ++m_gpuDisplayWaits;
        m_surface->requestGpuFrame();
        QTimer::singleShot(25, this, &PreviewPlayer::start);
        return;
    }
#endif
    const QByteArray data = m_data.toUtf8();
    megler_config config = {};
    config.data_dir = data.constData();
    config.package_dir = m_rootUtf8.constData();
    static int logSocket = LogChannel::openSender(QString::fromLocal8Bit(qgetenv("ANDROID_COMPATIBILITY_LAYER_CORE_LOG_SOCKET")));
    QString logError;
    if (!ErrorLog::startCoreChannel(&logError))
        qWarning().noquote() << "Android error log channel unavailable:" << logError;
    config.log = &coreLog;
    config.log_user = &logSocket;
    if (logSocket < 0 && !qgetenv("ANDROID_COMPATIBILITY_LAYER_CORE_LOG_SOCKET").isEmpty()) {
        qWarning() << "Core log display unavailable; application output remains in the IDE";
    }
    if (!megler_init(&config)) {
        setStatus(tr("Android runtime initialization failed."));
        ErrorLog::record(QStringLiteral("Android"), m_status, true);
        return;
    }

    megler_set_locale(m_localeUtf8.constData());
    const bool dark = m_theme == QLatin1String("dark") ||
                      (m_theme.isEmpty() && m_systemDark);
    megler_set_theme(m_systemAccent.isValid() ? m_systemAccent.rgba() : 0, dark);
    megler_run_config run = {};
    GoogleServicesRuntime googleServices(QString::fromUtf8(m_rootUtf8));
    QString googleError;
    if (!googleServices.apply(run, &googleError)) {
        qWarning().noquote() << "Google services:" << googleError;
        setStatus(tr("Google services: %1").arg(googleError));
        return;
    }
    run.apk_path = m_apkUtf8.constData();
    run.android_root = m_rootUtf8.constData();
    run.splits_dir = m_splitsUtf8.constData();
    run.width = m_surfaceWidth;
    run.height = m_surfaceHeight;
    run.density_dpi = m_density;
    run.gfx = previewGraphics();
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    bool gpuCanvasReady = false;
    bool remoteGpuCanvasReady = false;
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    constexpr char verifyParentVariable[] = "ACL_EMBEDDED_GPU_GBM_VERIFY_PARENT";
    const bool verifyParentEnabled = m_cpuPresentationOnly &&
                                     qEnvironmentVariableIntValue(verifyParentVariable) == 1;
    const bool gpuChannelConnected = m_remoteGpuTransport && m_remoteGpuTransport->connected();
    const auto logGbmProbe = [&](const QString &result) {
        const char *flag = verifyParentEnabled ? "enabled" :
                           (qEnvironmentVariableIsSet(verifyParentVariable) ? "disabled" : "unset");
        qInfo().noquote() << QStringLiteral("Embedded GPU GBM probe: verify_parent=%1; gpu_channel=%2; %3")
                                 .arg(QLatin1String(flag),
                                      QLatin1String(gpuChannelConnected ? "connected" : "unavailable"),
                                      result);
    };
    if (m_cpuPresentationOnly && m_remoteGpuCanvasPresent) {
        QOpenGLContext *context = new QOpenGLContext(this);
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(2, 0);
        context->setFormat(format);
        QString exportError;
        bool exportAvailable = false;
        if (context->create() && context->isOpenGLES()) {
            QOffscreenSurface probeSurface;
            probeSurface.setFormat(context->format());
            probeSurface.create();
            if (probeSurface.isValid() && context->makeCurrent(&probeSurface)) {
                exportAvailable = EmbeddedGpuTransport::canExportDmaBuf(&exportError);
                if (!exportAvailable) {
                    std::function<bool(const AuroraDmaBufFrame &, QString *)> verifyParent;
                    if (m_remoteGpuTransport && verifyParentEnabled) {
                        verifyParent = [this](const AuroraDmaBufFrame &frame, QString *error) {
                            const bool ok = m_remoteGpuTransport->sendFrame(frame, error);
                            // Do not leave a late reply on the stream after a timeout.
                            if (!ok) m_remoteGpuTransport->closePeer();
                            return ok;
                        };
                    }
                    logGbmProbe(EmbeddedGpuTransport::probeGbmRenderTarget(run.width, run.height, verifyParent));
                } else {
                    logGbmProbe(QStringLiteral("skipped: EGL DMA-BUF export is available"));
                }
                context->doneCurrent();
            } else {
                exportError = tr("Could not make the worker EGL context current for the DMA-BUF capability probe.");
                logGbmProbe(QStringLiteral("skipped: worker EGL context could not be made current"));
            }
        } else {
            exportError = tr("Could not create the worker GLES context.");
            logGbmProbe(QStringLiteral("skipped: worker GLES context unavailable"));
        }
        if (exportAvailable) {
            remoteGpuCanvasReady = m_gpuCanvas.prepareRemotePresentation(
                context, presentRemoteGpuCanvas, releaseRemoteGpuCanvas, this);
            gpuCanvasReady = remoteGpuCanvasReady;
        }
        if (!gpuCanvasReady) {
            delete context;
            if (exportError.isEmpty())
                exportError = tr("The DMA-BUF GPU Canvas could not be initialized.");
            // GPU readback still needs a working EGL binding on the drawing thread.
            qWarning().noquote() << "Embedded GPU DMA-BUF presentation unavailable; using CPU Canvas:"
                                 << exportError;
        }
    } else if (m_cpuPresentationOnly) {
        logGbmProbe(QStringLiteral("skipped: GPU channel was not negotiated"));
    }
#endif
    if (!m_cpuPresentationOnly) {
        gpuCanvasReady = m_gpuCanvas.prepare(m_gpuState->qtContext(), presentGpuCanvas, releaseGpuCanvas, this);
    }
    if (gpuCanvasReady) {
        run.gfx = previewGraphics(&m_gpuCanvas);
        if (remoteGpuCanvasReady)
            qInfo("Embedded GPU DMA-BUF producer is ready; waiting for the first live frame");
        else
            qInfo("Experimental GPU Canvas active; Android 2D draws into a shared GLES texture");
    } else if (!m_cpuPresentationOnly) {
        qWarning("Experimental GPU Canvas unavailable; using the CPU Canvas fallback");
    }
#endif
    if (m_audio) run.audio = m_audio->callbacks();
    run.codec = auroraMp3CodecCallbacks();
    run.media = auroraMp3MediaCallbacks();
    if (!m_deniedPermissions.contains(QStringLiteral("camera"))) run.camera = auroraCameraHost();
    run.sensors = auroraSensorHost();
    if (!m_deniedPermissions.contains(QStringLiteral("nearby_devices"))) run.bluetooth = auroraBluetoothHost();
    if (!m_deniedPermissions.contains(QStringLiteral("storage")) &&
        !m_deniedPermissions.contains(QStringLiteral("read_media_visual")) &&
        !m_deniedPermissions.contains(QStringLiteral("read_media_aural"))) run.storage = m_storage.host();
    if (!m_deniedPermissions.contains(QStringLiteral("location"))) run.location = auroraLocationHost();
    if (!m_deniedPermissions.contains(QStringLiteral("location"))) {
        if (m_location.isEmpty()) {
            auroraLocationUseDeviceLocation();
        } else {
            double latitude = 0, longitude = 0;
            if (!RuntimeSettings::parseLocation(m_location, &latitude, &longitude) ||
                !auroraLocationSetFixedLocation(latitude, longitude)) {
                setStatus(tr("Could not configure the location."));
                ErrorLog::record(QStringLiteral("Android"), m_status, true);
                return;
            }
        }
    }
    if (m_hasSystemHost) {
        run.system = m_systemHost;
        run.apps_dir = m_appsDirUtf8.constData();
    } else if (m_installHost) {
        run.system = m_installHost->host();
        run.apps_dir = m_appsDirUtf8.constData();
    } else if (m_picker) {
        run.system = m_picker->host();
    }
    if (m_keyboard) {
        m_keyboard->start();
        run.keyboard = m_keyboard->host();
    } else if (m_hasRemoteKeyboard) run.keyboard = m_remoteKeyboard;
    run.window.user = this;
    run.window.present_pixels = presentPixels;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    if (!m_cpuPresentationOnly) {
        run.present_egl_window = presentEglWindow;
        run.present_egl_layer = presentEglLayer;
        run.present_egl_views = presentEglViews;
        run.place_egl_layer = placeEglLayer;
    }
#endif
    run.on_exit = appExited;
    run.user = this;
    if (!megler_run(&run)) {
        if (m_keyboard) m_keyboard->stop();
        setStatus(tr("Could not start Android app: %1").arg(QString::fromUtf8(megler_last_error())));
        ErrorLog::record(QStringLiteral("Android"), m_status, true);
        return;
    }
    m_running = true;
    setStatus(tr("Waiting for the first frame..."));
}

void PreviewPlayer::setAudio(AuroraAudioOutput *audio)
{
    m_audio = audio;
}

void PreviewPlayer::setPicker(AuroraPickerHost *picker)
{
    m_picker = picker;
}

void PreviewPlayer::setInstallHost(InstallRequestHost *host)
{
    m_installHost = host;
    m_appsDirUtf8 = host ? host->appsDir().toUtf8() : QByteArray();
}

void PreviewPlayer::setSystemHost(const megler_system_host &host, const QString &appsDir)
{
    m_systemHost = host;
    m_hasSystemHost = true;
    m_appsDirUtf8 = appsDir.toUtf8();
}

void PreviewPlayer::setKeyboard(PreviewKeyboard *keyboard)
{
    m_keyboard = keyboard;
    if (m_keyboard) m_keyboard->setSurface(m_surface, QSize(m_surfaceWidth, m_surfaceHeight));
}

void PreviewPlayer::setRemoteKeyboard(const megler_keyboard_host &keyboard)
{
    m_remoteKeyboard = keyboard;
    m_hasRemoteKeyboard = true;
}

void PreviewPlayer::setSurface(PreviewSurface *surface)
{
    m_surface = surface;
    if (m_keyboard) m_keyboard->setSurface(surface, QSize(m_surfaceWidth, m_surfaceHeight));
}

void PreviewPlayer::setViewport(qreal width, qreal height, qreal pixelRatio)
{
    if (width <= 0 || height <= 0 || pixelRatio <= 0) return;
    const PreviewViewport viewport = previewViewport(width, height, pixelRatio, m_renderCap);
    const int surfaceWidth = viewport.pixels.width();
    const int surfaceHeight = viewport.pixels.height();
    const int density = viewport.density;
    if (surfaceWidth == m_surfaceWidth && surfaceHeight == m_surfaceHeight && density == m_density) return;
    m_surfaceWidth = surfaceWidth;
    m_surfaceHeight = surfaceHeight;
    m_density = density;
    if (m_keyboard) m_keyboard->setSurface(m_surface, QSize(surfaceWidth, surfaceHeight));
    if (m_running && !m_closing) megler_set_surface(surfaceWidth, surfaceHeight, density, 0);
}

void MEGLER_CB PreviewPlayer::presentPixels(void *user, const void *pixels, int width, int height, int stride)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    self->submitPixels(pixels, width, height, stride);
}

bool PreviewPlayer::submitPixels(const void *pixels, int width, int height, int stride)
{
    QImage copy = previewCopyFrame(pixels, width, height, stride);
    if (copy.isNull()) return false;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    m_gpuState->clear();
#endif
    QMutexLocker lock(&m_frameMutex);
    if (m_awaitingGestureFrame)
        m_gestureFrameChanged = (copy != m_gestureFrame);
    m_frame = copy;
    if (!m_framePending) {
        m_framePending = true;
        QMetaObject::invokeMethod(this, "publishFrame", Qt::QueuedConnection);
    }
    return true;
}

#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
bool MEGLER_CB PreviewPlayer::presentRemoteGpuCanvas(void *user, uint32_t texture, int width, int height)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    const bool transportAccepted = !self->m_remoteGpuCanvasFailed.loadAcquire() &&
        self->m_remoteGpuCanvasPresent &&
        self->m_remoteGpuCanvasPresent(self->m_remoteGpuCanvasUser, texture, width, height);
    if (!transportAccepted)
        self->m_remoteGpuCanvasFailed.storeRelease(1);
    if (self->m_remoteGpuCanvasFailed.loadAcquire()) {
        if (width <= 0 || height <= 0 || width > INT_MAX / 4 ||
            quint64(width) * quint64(height) > 8 * 1024 * 1024)
            return false;
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (!context || !context->isValid()) return false;
        QOpenGLFunctions *gl = context->functions();
        QImage pixels(width, height, QImage::Format_RGB32);
        if (pixels.isNull()) return false;
        QByteArray rgba(width * height * 4, char(0));
        for (int i = 0; i < 16 && gl->glGetError() != GL_NO_ERROR; ++i) {}
        GLint previousFramebuffer = 0;
        GLuint framebuffer = 0;
        gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
        gl->glGenFramebuffers(1, &framebuffer);
        if (!framebuffer) return false;
        gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const bool complete = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            gl->glFinish();
            gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
        const GLenum glError = gl->glGetError();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        gl->glDeleteFramebuffers(1, &framebuffer);
        if (!complete || glError != GL_NO_ERROR) return false;
        for (int y = 0; y < height; ++y) {
            const uchar *source = reinterpret_cast<const uchar *>(rgba.constData()) +
                                  (height - 1 - y) * width * 4;
            uchar *destination = pixels.scanLine(y);
            for (int x = 0; x < width; ++x) {
                destination[x * 4 + 0] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4 + 0];
                destination[x * 4 + 3] = source[x * 4 + 3];
            }
        }
        return self->submitPixels(pixels.constBits(), width, height, pixels.bytesPerLine());
    }
    QImage placeholder(width, height, QImage::Format_RGB32);
    if (placeholder.isNull()) return false;
    placeholder.fill(Qt::black);
    {
        QMutexLocker lock(&self->m_frameMutex);
        self->m_frame = placeholder;
        if (self->m_awaitingGestureFrame)
            self->m_gestureFrameChanged = true;
        if (!self->m_framePending) {
            self->m_framePending = true;
            QMetaObject::invokeMethod(self, "publishFrame", Qt::QueuedConnection);
        }
    }
    return true;
}

bool MEGLER_CB PreviewPlayer::releaseRemoteGpuCanvas(void *, uint32_t)
{
    return true;
}

bool MEGLER_CB PreviewPlayer::presentGpuCanvas(void *user, uint32_t texture, int width, int height)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    QImage placeholder;
    {
        QMutexLocker lock(&self->m_frameMutex);
        if (self->m_frame.size() != QSize(width, height)) {
            placeholder = QImage(width, height, QImage::Format_RGB32);
            if (placeholder.isNull())
                return false;
            placeholder.fill(Qt::black);
        }
    }
    if (!self->m_gpuState->presentCanvas(texture, width, height, self)) {
        static QAtomicInt warned = Q_BASIC_ATOMIC_INITIALIZER(0);
        if (warned.testAndSetOrdered(0, 1))
            qWarning("Experimental GPU Canvas presentation failed; using the CPU frame fallback");
        return false;
    }
    QMutexLocker lock(&self->m_frameMutex);
    if (!placeholder.isNull())
        self->m_frame = placeholder;
    if (self->m_awaitingGestureFrame)
        self->m_gestureFrameChanged = true;
    if (!self->m_framePending) {
        self->m_framePending = true;
        QMetaObject::invokeMethod(self, "publishFrame", Qt::QueuedConnection);
    }
    return true;
}

bool MEGLER_CB PreviewPlayer::releaseGpuCanvas(void *user, uint32_t texture)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    return self->m_gpuState->releaseCanvasTexture(texture, self);
}
#endif

int MEGLER_CB PreviewPlayer::presentEglWindow(void *user, void *display, void *surface, int width, int height)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    const int result = self->m_gpuState->submit(display, surface, width, height, self);
    if (result != 1) {
        static QAtomicInt warned = Q_BASIC_ATOMIC_INITIALIZER(0);
        if (result == 0 && warned.testAndSetOrdered(0, 1)) {
            const QString reason = self->m_gpuState->lastFailure();
            qWarning().noquote() << "Experimental GPU presentation unavailable:"
                                 << (reason.isEmpty() ? QStringLiteral("unknown reason") : reason)
                                 << "; using CPU fallback";
        }
        return result;
    }
    {
        QMutexLocker lock(&self->m_frameMutex);
        if (self->m_frame.size() != QSize(width, height)) {
            self->m_frame = QImage(width, height, QImage::Format_RGB32);
            if (self->m_frame.isNull()) {
                self->m_gpuState->clear();
                return 0;
            }
            self->m_frame.fill(Qt::black);
        }
        if (!self->m_framePending) {
            self->m_framePending = true;
            QMetaObject::invokeMethod(self, "publishFrame", Qt::QueuedConnection);
        }
    }
    static QAtomicInt reported = Q_BASIC_ATOMIC_INITIALIZER(0);
    if (reported.testAndSetOrdered(0, 1))
        qInfo("Experimental GPU presentation active; Qt consumed an EGLImage");
    return 1;
}

int MEGLER_CB PreviewPlayer::presentEglLayer(void *user, int layer, void *display, void *surface, int width, int height)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    return self->m_gpuState->submit(display, surface, width, height, self, layer);
}

bool MEGLER_CB PreviewPlayer::presentEglViews(void *user, const void *pixels, int width, int height, int stride)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    if (!self->m_gpuState->setViews(pixels, width, height, stride, self)) return false;
    QMutexLocker lock(&self->m_frameMutex);
    if (self->m_frame.size() != QSize(width, height)) {
        self->m_frame = QImage(width, height, QImage::Format_RGB32);
        if (self->m_frame.isNull()) {
            self->m_gpuState->clear();
            return false;
        }
        self->m_frame.fill(Qt::black);
    }
    if (!self->m_framePending) {
        self->m_framePending = true;
        QMetaObject::invokeMethod(self, "publishFrame", Qt::QueuedConnection);
    }
    return true;
}

void MEGLER_CB PreviewPlayer::placeEglLayer(void *user, int layer, int x, int y, int width, int height, int flags)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    self->m_gpuState->place(layer, x, y, width, height, flags, self);
}

void PreviewPlayer::requestGpuFrame()
{
    if (m_surface) m_surface->requestGpuFrame();
}
#endif

void PreviewPlayer::publishFrame()
{
    {
        QMutexLocker lock(&m_frameMutex);
        m_framePending = false;
    }
    if (m_running && !m_closing && m_status != tr("Android app is running."))
        setStatus(tr("Android app is running."));
    emit frameChanged();
}

void MEGLER_CB PreviewPlayer::appExited(void *user, int status, const char *reason)
{
    PreviewPlayer *self = static_cast<PreviewPlayer *>(user);
    QMetaObject::invokeMethod(self, "ended", Qt::QueuedConnection,
                              Q_ARG(int, status), Q_ARG(QString, QString::fromUtf8(reason ? reason : "")));
}

void PreviewPlayer::ended(int status, const QString &reason)
{
    m_running = false;
    m_exitStatus = status;
    if (m_picker) m_picker->stop();
    if (m_keyboard) m_keyboard->stop();
    if (!m_closing) {
        setStatus(reason.isEmpty() ? tr("Android app ended (status %1).").arg(status)
                                   : tr("Android app ended: %1").arg(reason));
        if (status != 0) ErrorLog::record(QStringLiteral("Android"), m_status, true);
    }
    if (m_closing || m_closeOnExit) emit finished();
}

bool PreviewPlayer::touch(int phase, float x, float y)
{
    if (!m_running || m_closing) return false;
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_frame.size() != QSize(m_surfaceWidth, m_surfaceHeight)) return false;
    }
    megler_touch(phase, 1, x, y);
    return true;
}

void PreviewPlayer::back()
{
    if (!m_running || m_closing) return;
    if (m_keyboard && m_keyboard->back()) return;
    megler_key(4, true);
    megler_key(4, false);
}

bool PreviewPlayer::gestureBack()
{
    if (!m_running || m_closing) return false;
    {
        QMutexLocker lock(&m_frameMutex);
        m_gestureFrame = m_frame;
        m_gestureFrameChanged = false;
        m_awaitingGestureFrame = true;
    }
    back();
    return true;
}

bool PreviewPlayer::hasFrameAfterGestureBack() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_awaitingGestureFrame && m_gestureFrameChanged;
}

void PreviewPlayer::endGestureFrameWait()
{
    QMutexLocker lock(&m_frameMutex);
    m_awaitingGestureFrame = false;
    m_gestureFrameChanged = false;
    m_gestureFrame = QImage();
}

void PreviewPlayer::closeToManager()
{
    if (!m_closeOnBackGesture || m_closing) return;
    emit returnToManagerRequested();
    close();
}

void PreviewPlayer::close()
{
    if (m_closing) return;
    m_closing = true;
    if (m_picker) m_picker->stop();
    if (m_keyboard) m_keyboard->stop();
    if (!m_running) {
        emit finished();
        return;
    }
    setStatus(tr("Stopping Android app..."));
    QTimer::singleShot(5000, this, [this]() {
        if (m_running) {
            qCritical() << "Test app did not stop in time.";
            std::_Exit(EXIT_FAILURE);
        }
    });
    megler_stop();
}

PreviewSurface::PreviewSurface(QQuickItem *parent) : QQuickPaintedItem(parent), m_wheelTimer(new QTimer(this))
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::BackButton);
    m_wheelTimer->setInterval(16);
    connect(m_wheelTimer, &QTimer::timeout, this, &PreviewSurface::advanceWheelGesture);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        cancelWheelGesture();
        if (window) {
            connect(window, &QWindow::screenChanged, this, [this, window]() {
                if (window == this->window()) { cancelWheelGesture(); updateViewport(); }
            });
            connect(window, &QWindow::activeChanged, this, [this, window]() {
                if (window == this->window() && !window->isActive()) cancelWheelGesture();
            });
        }
        updateViewport();
    });
}

PreviewSurface::~PreviewSurface()
{
    cancelWheelGesture();
}

void PreviewSurface::setPlayer(QObject *object)
{
    PreviewPlayer *player = qobject_cast<PreviewPlayer *>(object);
    if (m_player == player) return;
    cancelWheelGesture();
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    delete m_gpuItem;
    m_gpuItem = nullptr;
#endif
    if (m_player) {
        m_player->setSurface(nullptr);
        disconnect(m_player, nullptr, this, nullptr);
    }
    m_player = player;
    if (m_player) {
        m_player->setSurface(this);
        connect(m_player, &PreviewPlayer::frameChanged, this, [this]() { update(); });
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
        if (m_player->usesGpuSurface()) {
            m_gpuItem = new PreviewGpuItem(m_player->gpuState(), this);
            connect(m_player, &PreviewPlayer::frameChanged, m_gpuItem, &QQuickItem::update);
        }
#endif
    }
    updateViewport();
    update();
    emit playerChanged();
}

void PreviewSurface::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    if (newGeometry.size() != oldGeometry.size()) cancelWheelGesture();
    QQuickPaintedItem::geometryChanged(newGeometry, oldGeometry);
    updateViewport();
}

void PreviewSurface::updateViewport()
{
    if (width() <= 0 || height() <= 0) return;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    if (m_gpuItem) {
        m_gpuItem->setWidth(width());
        m_gpuItem->setHeight(height());
        m_gpuItem->update();
    }
#endif
    const qreal ratio = window() ? window()->devicePixelRatio() : 1;
    setTextureSize(previewViewport(width(), height(), ratio, m_player ? m_player->renderCap() : 0).pixels);
    if (m_player) m_player->setViewport(width(), height(), ratio);
}

#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
void PreviewSurface::requestGpuFrame()
{
    if (m_gpuItem) m_gpuItem->update();
}
#endif

void PreviewSurface::paint(QPainter *painter)
{
    painter->fillRect(boundingRect(), Qt::black);
    if (!m_player) return;
    const QImage image = m_player->frame();
    if (image.isNull()) return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(imageTarget(image), image);
}

QRectF PreviewSurface::imageTarget(const QImage &image) const
{
    const QSizeF size = image.size().scaled(boundingRect().size().toSize(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
}

bool PreviewSurface::sendTouch(int phase, const QPointF &point)
{
    if (!m_player) return false;
    const QImage image = m_player->frame();
    if (image.isNull() || width() <= 0 || height() <= 0) return false;
    const QRectF target = imageTarget(image);
    if (target.isEmpty()) return false;
    if (phase == 0 && !target.contains(point)) return false;
    const float x = qBound(qreal(0), (point.x() - target.left()) * image.width() / target.width(), qreal(image.width() - 1));
    const float y = qBound(qreal(0), (point.y() - target.top()) * image.height() / target.height(), qreal(image.height() - 1));
    return m_player->touch(phase, x, y);
}

void PreviewSurface::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::BackButton) {
        if (!m_backPressed && m_player) {
            m_backPressed = true;
            m_player->back();
        }
        event->accept();
        return;
    }
    cancelWheelGesture();
    m_lastPoint = event->localPos();
    m_pressed = sendTouch(0, m_lastPoint);
    const bool outsideEdge = m_lastPoint.x() >= m_edgeZoneWidth &&
                             m_lastPoint.x() < width() - m_edgeZoneWidth;
    setKeepMouseGrab(outsideEdge);
    if (m_pressed || outsideEdge) event->accept(); else event->ignore();
}

void PreviewSurface::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressed) {
        if (keepMouseGrab()) event->accept();
        return;
    }
    m_lastPoint = event->localPos();
    sendTouch(1, m_lastPoint);
    event->accept();
}

void PreviewSurface::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::BackButton) {
        m_backPressed = false;
        event->accept();
        return;
    }
    if (!m_pressed) {
        if (keepMouseGrab()) {
            setKeepMouseGrab(false);
            event->accept();
        }
        return;
    }
    m_lastPoint = event->localPos();
    sendTouch(2, m_lastPoint);
    m_pressed = false;
    setKeepMouseGrab(false);
    event->accept();
}

void PreviewSurface::mouseUngrabEvent()
{
    m_backPressed = false;
    if (m_pressed) sendTouch(3, m_lastPoint);
    m_pressed = false;
    setKeepMouseGrab(false);
}

void PreviewSurface::wheelEvent(QWheelEvent *event)
{
    if (m_pressed || event->buttons() != Qt::NoButton || !m_player) { event->ignore(); return; }
    const QImage image = m_player->frame();
    const QPointF point = event->posF();
    if (image.isNull() || !imageTarget(image).contains(point)) { event->ignore(); return; }
    QPointF delta = event->pixelDelta().isNull() ? QPointF(event->angleDelta()) / 120.0
                                                 : QPointF(event->pixelDelta()) / 40.0;
    if (delta.isNull()) { event->ignore(); return; }
    m_wheelPoint = point;
    m_wheelPending.setX(qBound(qreal(-6), m_wheelPending.x() + delta.x(), qreal(6)));
    m_wheelPending.setY(qBound(qreal(-6), m_wheelPending.y() + delta.y(), qreal(6)));
    if (!m_wheelActive) startWheelGesture();
    event->accept();
}

void PreviewSurface::startWheelGesture()
{
    if (!m_player || m_pressed) { cancelWheelGesture(); return; }
    const bool horizontal = qAbs(m_wheelPending.x()) > qAbs(m_wheelPending.y());
    const qreal delta = horizontal ? m_wheelPending.x() : m_wheelPending.y();
    if (qAbs(delta) < 0.25) return;
    const qreal used = qBound(qreal(-1), delta, qreal(1));
    if (horizontal) m_wheelPending.rx() -= used; else m_wheelPending.ry() -= used;
    m_wheelStart = m_wheelPoint;
    if (!previewWheelGesture(imageTarget(m_player->frame()), m_wheelStart, horizontal, used, &m_wheelEnd) ||
        !sendTouch(0, m_wheelStart)) {
        cancelWheelGesture();
        return;
    }
    m_wheelLast = m_wheelStart;
    m_wheelStep = 0;
    m_wheelActive = true;
    m_wheelTimer->start();
}

void PreviewSurface::advanceWheelGesture()
{
    if (!m_wheelActive) return;
    ++m_wheelStep;
    m_wheelLast = m_wheelStart + (m_wheelEnd - m_wheelStart) * (m_wheelStep / 4.0);
    if (!sendTouch(1, m_wheelLast)) { cancelWheelGesture(); return; }
    if (m_wheelStep == 4) {
        sendTouch(2, m_wheelLast);
        m_wheelTimer->stop();
        m_wheelActive = false;
        startWheelGesture();
    }
}

void PreviewSurface::cancelWheelGesture()
{
    m_wheelTimer->stop();
    if (m_wheelActive) sendTouch(3, m_wheelLast);
    m_wheelActive = false;
    m_wheelPending = QPointF();
}
