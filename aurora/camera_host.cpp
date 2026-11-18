#include "camera_host.h"
#include "camera_qt_frame.h"

#include <QAbstractVideoSurface>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#include <QWaitCondition>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>

namespace {

struct ProbeStats {
    QSize size;
    int frames = 0;
    qint64 firstUs = 0;
    qint64 lastUs = 0;

    void observe(const QSize &frameSize, qint64 timeUs)
    {
        if (size != frameSize || (frames && timeUs <= lastUs)) {
            size = frameSize;
            frames = 0;
        }
        if (!frames) firstUs = timeUs;
        lastUs = timeUs;
        ++frames;
    }

    int observedFps() const
    {
        if (frames < 3 || lastUs - firstUs < 200000) return 0;
        const qreal fps = 1000000.0 * (frames - 1) / (lastUs - firstUs);
        return std::isfinite(fps) && fps >= 0.5 && fps <= 1000 ? qRound(fps) : 0;
    }
};

bool validSize(const QSize &size);
int outputFactor(const QSize &native);
bool supportedFormat(QVideoFrame::PixelFormat format)
{
    return format == QVideoFrame::Format_NV12 || format == QVideoFrame::Format_NV21 ||
           format == QVideoFrame::Format_YUV420P || format == QVideoFrame::Format_YV12;
}

class Surface : public QAbstractVideoSurface {
public:
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const override
    {
        if (type != QAbstractVideoBuffer::NoHandle) return {};
        return {QVideoFrame::Format_NV12, QVideoFrame::Format_NV21,
                QVideoFrame::Format_YUV420P, QVideoFrame::Format_YV12};
    }

    bool start(const QVideoSurfaceFormat &format) override
    {
        qWarning() << "camera: negotiated surface" << format.frameSize()
                   << "pixel format" << int(format.pixelFormat())
                   << "handle" << int(format.handleType()) << "fps" << format.frameRate();
        return QAbstractVideoSurface::start(format);
    }

    void beginProbe()
    {
        QMutexLocker lock(&mutex);
        probe = ProbeStats();
        probeError.clear();
        probing = true;
        clock.start();
    }

    ProbeStats probeState(QString *error)
    {
        QMutexLocker lock(&mutex);
        if (error) *error = probeError;
        return probe;
    }

    void endProbe()
    {
        QMutexLocker lock(&mutex);
        probing = false;
    }

    void enable(megler_camera_frame_fn fn, void *core, int width, int height)
    {
        QMutexLocker lock(&mutex);
        callback = fn;
        callbackCore = core;
        expected = QSize(width, height);
        clock.start();
    }

    void disable()
    {
        QMutexLocker lock(&mutex);
        callback = nullptr;
        callbackCore = nullptr;
    }

    void fail(const char *reason)
    {
        QMutexLocker lock(&mutex);
        if (!callback) return;
        qWarning() << "camera:" << reason;
        megler_camera_frame_fn fn = callback;
        callback = nullptr;
        fn(callbackCore, nullptr);
    }

    bool present(const QVideoFrame &input) override
    {
        QVideoFrame frame(input);
        QMutexLocker lock(&mutex);
        if (!callback && !probing) return true;
        const int factor = outputFactor(frame.size());
        const QSize outSize = factor ? frame.size() / factor : QSize();
        const bool sizeOk = factor > 0;
        const bool matchOk = probing || outSize == expected;
        const bool formatOk = supportedFormat(frame.pixelFormat());
        const bool mapOk = sizeOk && matchOk && formatOk && frame.map(QAbstractVideoBuffer::ReadOnly);
        if (!mapOk) {
            qWarning() << "camera: frame size" << frame.size() << "format" << int(frame.pixelFormat())
                       << "handle" << int(frame.handleType()) << "expected" << expected
                       << "sizeOk" << sizeOk << "matchesExpected" << matchOk
                       << "formatOk" << formatOk
                       << "mapAttempted" << (sizeOk && matchOk && formatOk) << "mapped" << mapOk;
            endWithError(sizeOk && matchOk && formatOk ? "viewfinder frame could not be mapped to CPU memory"
                                                       : "unsupported or mismatched viewfinder frame");
            return false;
        }

        const uint8_t *y = frame.bits(0);
        const uint8_t *uv = frame.planeCount() >= 2 ? frame.bits(1) : nullptr;
        const int stride = frame.bytesPerLine(0);
        const int uvStride = frame.planeCount() >= 2 ? frame.bytesPerLine(1) : stride;
        const int mapped = frame.mappedBytes();
        const uintptr_t base = reinterpret_cast<uintptr_t>(y);
        const uintptr_t chroma = reinterpret_cast<uintptr_t>(uv);
        const bool ok = aurora_camera::copyMappedFrame(frame, &pixels, factor);
        if (!ok) {
            qWarning() << "camera: mapped frame layout"
                       << "planes" << frame.planeCount() << "bytes" << mapped
                       << "stride0" << stride << "stride1" << uvStride
                       << "yPresent" << (y != nullptr) << "uvPresent" << (uv != nullptr)
                       << "uvOffset" << (uv && chroma >= base ? qint64(chroma - base) : qint64(-1));
            frame.unmap();
            endWithError("unsupported frame layout or invalid plane bounds");
            return false;
        }
        if (probing) {
            probe.observe(outSize, clock.nsecsElapsed() / 1000);
            frame.unmap();
            return true;
        }
        megler_camera_frame picture = {};
        picture.width = outSize.width();
        picture.height = outSize.height();
        picture.y = pixels.data();
        picture.uv = pixels.data() + static_cast<size_t>(picture.width) * picture.height;
        picture.y_stride = picture.uv_stride = picture.width;
        picture.time_us = clock.nsecsElapsed() / 1000;
        callback(callbackCore, &picture);
        frame.unmap();
        return true;
    }

private:
    void endWithError(const char *reason)
    {
        qWarning() << "camera:" << reason;
        if (probing) {
            probeError = QString::fromLatin1(reason);
            probing = false;
            return;
        }
        megler_camera_frame_fn fn = callback;
        callback = nullptr;
        fn(callbackCore, nullptr);
    }

    QMutex mutex;
    megler_camera_frame_fn callback = nullptr;
    void *callbackCore = nullptr;
    QSize expected;
    QElapsedTimer clock;
    std::vector<uint8_t> pixels;
    ProbeStats probe;
    QString probeError;
    bool probing = false;
};

struct Camera {
    QCamera *device = nullptr;
    Surface *surface = nullptr;
    QString name;
    QList<QCameraViewfinderSettings> settings;
    megler_camera_caps caps = {};
    bool streaming = false;
    bool observedDefault = false;
};

bool validSize(const QSize &size)
{
    return size.width() > 0 && size.height() > 0 && !(size.width() & 1) && !(size.height() & 1) &&
           size.width() <= 1920 && size.height() <= 1920 &&
           static_cast<qint64>(size.width()) * size.height() <= 1920 * 1080;
}

// Native frames up to 4K are accepted, but frames above 1080p-class are halved in each
// direction so the copy, memory and Megler-side work stay bounded. Returns 0 if unusable.
int outputFactor(const QSize &native)
{
    if (native.width() <= 0 || native.height() <= 0 || (native.width() & 1) || (native.height() & 1) ||
        native.width() > 3840 || native.height() > 3840 ||
        static_cast<qint64>(native.width()) * native.height() > 3840 * 2160)
        return 0;
    if (validSize(native)) return 1;
    if (native.width() % 4 || native.height() % 4) return 0;
    return validSize(native / 2) ? 2 : 0;
}

bool capsFromProbe(const ProbeStats &stats, megler_camera_caps *caps)
{
    // This is an observed delivery rate, not a claim about hardware frame-rate ranges.
    const int fps = stats.observedFps();
    if (!fps || !validSize(stats.size)) return false;
    caps->size_count = 1;
    caps->sizes[0] = {stats.size.width(), stats.size.height(), fps, fps};
    return true;
}

const char *settingRejection(const QCameraViewfinderSettings &s)
{
    if (!validSize(s.resolution())) return "invalid, odd or oversized resolution";
    if (!supportedFormat(s.pixelFormat()) && s.pixelFormat() != QVideoFrame::Format_Invalid)
        return "unsupported pixel format";
    const qreal min = s.minimumFrameRate(), max = s.maximumFrameRate();
    if (!std::isfinite(min) || !std::isfinite(max) || min < 0 || min > max || max < 1 || max > 1000)
        return "missing or invalid frame rate";
    if (std::max(1, static_cast<int>(std::ceil(min))) > static_cast<int>(std::floor(max)))
        return "frame rate has no whole-fps value";
    return nullptr;
}

void collectCaps(QList<QCameraViewfinderSettings> *settings, megler_camera_caps *caps)
{
    std::sort(settings->begin(), settings->end(), [](const QCameraViewfinderSettings &a,
                                                      const QCameraViewfinderSettings &b) {
        return static_cast<qint64>(a.resolution().width()) * a.resolution().height() >
               static_cast<qint64>(b.resolution().width()) * b.resolution().height();
    });
    for (const QCameraViewfinderSettings &s : *settings) {
        const QSize size = s.resolution();
        if (settingRejection(s)) continue;
        const int minFps = std::max(1, static_cast<int>(std::ceil(s.minimumFrameRate())));
        const int maxFps = static_cast<int>(std::floor(s.maximumFrameRate()));
        if (minFps > maxFps) continue;
        bool found = false;
        for (int i = 0; i < caps->size_count; ++i) {
            megler_camera_size &existing = caps->sizes[i];
            if (existing.width == size.width() && existing.height == size.height()) {
                existing.min_fps = std::min(existing.min_fps, minFps);
                existing.max_fps = std::max(existing.max_fps, maxFps);
                found = true;
                break;
            }
        }
        if (!found && caps->size_count < MEGLER_CAMERA_MAX_SIZES) {
            megler_camera_size &entry = caps->sizes[caps->size_count++];
            entry.width = size.width();
            entry.height = size.height();
            entry.min_fps = minFps;
            entry.max_fps = maxFps;
        }
    }
}

class Worker : public QObject {
    Q_OBJECT
public:
    bool call(const std::function<void()> &fn)
    {
        std::lock_guard<std::mutex> serial(serialMutex);
        if (shutDown) return false;
        dispatch(fn);
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> serial(serialMutex);
        if (shutDown) return;
        dispatch([this] {
            for (Camera *camera : cameras) {
                camera->surface->fail("camera host is shutting down");
                release(camera);
            }
            opened.clear();
            available.clear();
        });
        shutDown = true;
        thread()->quit();
        thread()->wait();
    }

    void closeHandle(Camera *camera)
    {
        std::lock_guard<std::mutex> serial(serialMutex);
        if (!cameras.contains(camera)) return;
        if (!shutDown) {
            dispatch([this, camera] { close(camera); });
        } else {
            cameras.remove(camera);
            delete camera;
        }
    }

    void dispatch(const std::function<void()> &fn)
    {
        QMutexLocker lock(&jobMutex);
        job = fn;
        done = false;
        if (!QMetaObject::invokeMethod(this, "runJob", Qt::QueuedConnection))
            qFatal("camera: could not dispatch to camera thread");
        while (!done) jobDone.wait(&jobMutex);
    }

    QList<QCameraInfo> devices()
    {
        if (!enumerated) {
            QCamera probe;
            available = QCameraInfo::availableCameras();
            if (available.isEmpty())
                qWarning() << "camera: Qt reported no cameras; check the backend and Camera permission";
            std::stable_sort(available.begin(), available.end(), [](const QCameraInfo &a, const QCameraInfo &b) {
                return a.position() == QCamera::BackFace && b.position() != QCamera::BackFace;
            });
            enumerated = true;
        }
        return available;
    }

    bool waitFor(QCamera *camera, QCamera::Status wanted)
    {
        if (camera->status() == wanted) return true;
        if (camera->error() != QCamera::NoError) return false;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(camera, &QCamera::statusChanged, &loop, [&](QCamera::Status status) {
            if (status == wanted) loop.quit();
        });
        connect(camera, static_cast<void (QCamera::*)(QCamera::Error)>(&QCamera::error),
                &loop, &QEventLoop::quit);
        timer.start(10000);
        loop.exec();
        return camera->status() == wanted && camera->error() == QCamera::NoError;
    }

    bool probeDefault(Camera *c, megler_camera_caps *caps)
    {
        qWarning() << "camera: trying the backend's default viewfinder; no capability is assumed";
        c->surface->beginProbe();
        c->device->start();
        const bool active = waitFor(c->device, QCamera::ActiveStatus);
        QString failure;
        if (active) {
            const QCameraViewfinderSettings actual = c->device->viewfinderSettings();
            qWarning() << "camera: active viewfinder reports" << actual.resolution()
                       << "format" << int(actual.pixelFormat()) << "fps"
                       << actual.minimumFrameRate() << actual.maximumFrameRate();
            QEventLoop loop;
            QTimer poll, timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            connect(&poll, &QTimer::timeout, &loop, [&] {
                QString error;
                const ProbeStats stats = c->surface->probeState(&error);
                if (stats.observedFps() || !error.isEmpty() || c->device->error() != QCamera::NoError)
                    loop.quit();
            });
            poll.start(25);
            timeout.start(5000);
            loop.exec();
        } else {
            failure = QStringLiteral("default viewfinder failed to start: %1").arg(c->device->errorString());
        }
        QString probeError;
        const ProbeStats stats = c->surface->probeState(&probeError);
        const int fps = stats.observedFps();
        c->surface->endProbe();
        if (failure.isEmpty() && !probeError.isEmpty()) failure = probeError;
        if (failure.isEmpty() && c->device->error() != QCamera::NoError)
            failure = QStringLiteral("backend error %1: %2").arg(int(c->device->error())).arg(c->device->errorString());
        c->device->stop();
        const bool stopped = waitFor(c->device, QCamera::LoadedStatus);
        if (!stopped && failure.isEmpty()) failure = QStringLiteral("default viewfinder failed to stop");
        if (!active || !failure.isEmpty() || !capsFromProbe(stats, caps)) {
            if (failure.isEmpty()) failure = QStringLiteral("not enough usable default viewfinder frames in 5 seconds");
            const QByteArray text = failure.toUtf8();
            std::snprintf(caps->why, sizeof(caps->why), "Camera probe failed: %s", text.constData());
            qWarning() << "camera:" << caps->why << "frames" << stats.frames
                       << "size" << stats.size;
            return false;
        }
        c->observedDefault = true;
        qWarning() << "camera: default viewfinder observed" << stats.size << "approximately" << fps
                   << "fps from" << stats.frames << "frames; other modes and rates remain unknown";
        return true;
    }

    Camera *open(int index, megler_camera_caps *caps, int *error)
    {
        std::memset(caps, 0, sizeof(*caps));
        *error = MEGLER_CAMERA_FAILED;
        const QList<QCameraInfo> list = devices();
        if (index < 0 || index >= list.size()) {
            std::snprintf(caps->why, sizeof(caps->why), "No such camera");
            return nullptr;
        }
        const QCameraInfo info = list[index];
        if (opened.contains(info.deviceName())) {
            *error = MEGLER_CAMERA_BUSY;
            std::snprintf(caps->why, sizeof(caps->why), "Camera is already open in this process");
            return nullptr;
        }
        Camera *c = new Camera;
        c->name = info.deviceName();
        c->surface = new Surface;
        c->device = new QCamera(info);
        c->device->setViewfinder(c->surface);
        c->device->load();
        if (!waitFor(c->device, QCamera::LoadedStatus)) {
            const QByteArray why = c->device->errorString().toUtf8();
            std::snprintf(caps->why, sizeof(caps->why), "Camera load failed or timed out: %s", why.constData());
            qWarning() << "camera:" << caps->why;
            delete c->device;
            delete c->surface;
            delete c;
            return nullptr;
        }
        c->settings = c->device->supportedViewfinderSettings();
        qWarning() << "camera: Qt reported" << c->settings.size() << "viewfinder settings";
        for (const QCameraViewfinderSettings &s : c->settings) {
            qWarning() << "camera: Qt setting" << s.resolution() << "format" << int(s.pixelFormat())
                       << "fps" << s.minimumFrameRate() << s.maximumFrameRate()
                       << (settingRejection(s) ? settingRejection(s) : "usable");
        }
        collectCaps(&c->settings, caps);
        qWarning() << "camera: usable reported modes:" << caps->size_count;
        if (!caps->size_count && !probeDefault(c, caps)) {
            delete c->device;
            delete c->surface;
            delete c;
            return nullptr;
        }
        c->caps = *caps;
        connect(c->device, static_cast<void (QCamera::*)(QCamera::Error)>(&QCamera::error),
                c->device, [c](QCamera::Error) {
            qWarning() << "camera: backend error:" << c->device->errorString();
            c->surface->fail("camera backend stopped");
        });
        connect(c->device, &QCamera::statusChanged, c->device, [c](QCamera::Status status) {
            if (c->streaming && (status == QCamera::UnavailableStatus || status == QCamera::UnloadedStatus))
                c->surface->fail("camera became unavailable");
        });
        opened.insert(c->name);
        cameras.insert(c);
        *error = MEGLER_CAMERA_OK;
        return c;
    }

    bool start(Camera *c, int width, int height, int fps, megler_camera_frame_fn frame, void *core)
    {
        if (!c || !frame || fps <= 0 || c->streaming) return false;
        bool advertised = false;
        for (int i = 0; i < c->caps.size_count; ++i)
            if (c->caps.sizes[i].width == width && c->caps.sizes[i].height == height) advertised = true;
        if (!advertised) return false;
        if (c->observedDefault) {
            qWarning() << "camera: starting previously observed default viewfinder";
        } else {
            const QCameraViewfinderSettings *best = nullptr;
            qreal distance = std::numeric_limits<qreal>::max();
            for (const QCameraViewfinderSettings &s : c->settings) {
                if (s.resolution() != QSize(width, height) || settingRejection(s))
                    continue;
                const qreal d = fps < s.minimumFrameRate() ? s.minimumFrameRate() - fps :
                                fps > s.maximumFrameRate() ? fps - s.maximumFrameRate() : 0;
                if (d < distance) { best = &s; distance = d; }
            }
            if (!best) return false;
            c->device->setViewfinderSettings(*best);
        }
        c->device->start();
        if (!waitFor(c->device, QCamera::ActiveStatus)) {
            qWarning() << "camera: start failed or timed out:" << c->device->errorString();
            c->surface->disable();
            c->device->stop();
            return false;
        }
        const QCameraViewfinderSettings actual = c->device->viewfinderSettings();
        qWarning() << "camera: started viewfinder reports" << actual.resolution()
                   << "format" << int(actual.pixelFormat()) << "fps"
                   << actual.minimumFrameRate() << actual.maximumFrameRate();
        c->surface->enable(frame, core, width, height);
        c->streaming = true;
        return true;
    }

    void stop(Camera *c)
    {
        if (!c) return;
        c->surface->disable();
        c->device->stop();
        c->streaming = false;
    }

    void close(Camera *c)
    {
        if (!c) return;
        opened.remove(c->name);
        cameras.remove(c);
        release(c);
        delete c;
    }

#ifdef AURORA_CAMERA_TEST
    void addTestCamera(Camera *c) { cameras.insert(c); }
#endif

private slots:
    void runJob()
    {
        std::function<void()> current;
        {
            QMutexLocker lock(&jobMutex);
            current = job;
        }
        current();
        QMutexLocker lock(&jobMutex);
        job = {};
        done = true;
        jobDone.wakeAll();
    }

private:
    void release(Camera *c)
    {
        stop(c);
        delete c->device;
        c->device = nullptr;
        c->surface->disable();
        delete c->surface;
        c->surface = nullptr;
        c->settings.clear();
        c->name.clear();
    }

    std::mutex serialMutex;
    QMutex jobMutex;
    QWaitCondition jobDone;
    std::function<void()> job;
    bool done = false;
    QSet<QString> opened;
    QSet<Camera *> cameras;
    QList<QCameraInfo> available;
    bool enumerated = false;
    bool shutDown = false;
};

Worker *worker()
{
    static Worker *instance = [] {
        QThread *thread = new QThread;
        Worker *w = new Worker;
        w->moveToThread(thread);
        thread->start();
        return w;
    }();
    return instance;
}

int MEGLER_CB count(void *user)
{
    int result = 0;
    static_cast<Worker *>(user)->call([&] { result = static_cast<Worker *>(user)->devices().size(); });
    return result;
}

bool MEGLER_CB info(void *user, int index, megler_camera_info *out)
{
    if (!out) return false;
    bool result = false;
    static_cast<Worker *>(user)->call([&] {
        const QList<QCameraInfo> list = static_cast<Worker *>(user)->devices();
        if (index < 0 || index >= list.size()) return;
        const QCameraInfo &device = list[index];
        std::memset(out, 0, sizeof(*out));
        if (device.position() == QCamera::UnspecifiedPosition)
            qWarning() << "camera: unknown facing, using back:" << device.deviceName();
        out->facing = device.position() == QCamera::FrontFace ? MEGLER_CAMERA_FRONT : MEGLER_CAMERA_BACK;
        const int angle = device.orientation();
        const bool landscape = QGuiApplication::instance() && QGuiApplication::primaryScreen() &&
                               (QGuiApplication::primaryScreen()->nativeOrientation() == Qt::LandscapeOrientation ||
                                QGuiApplication::primaryScreen()->nativeOrientation() == Qt::InvertedLandscapeOrientation);
        const int turned = aurora_camera::sensorOrientation(angle, landscape);
        if (turned < 0) {
            qWarning() << "camera: unknown sensor orientation, using zero:" << angle;
            out->orientation = 0;
        } else {
            out->orientation = turned;
        }
        qWarning() << "camera: sensor angle" << angle << "natural landscape" << landscape
                   << "reported turn" << out->orientation;
        out->focus = out->light = -1;
        const QByteArray name = device.description().toUtf8();
        std::snprintf(out->name, sizeof(out->name), "%s", name.constData());
        result = true;
    });
    return result;
}

void *MEGLER_CB open(void *user, int index, megler_camera_caps *caps, int *error)
{
    if (!caps || !error) {
        qWarning() << "camera: open requires caps and error";
        return nullptr;
    }
    Camera *result = nullptr;
    if (!static_cast<Worker *>(user)->call([&] { result = static_cast<Worker *>(user)->open(index, caps, error); })) {
        std::memset(caps, 0, sizeof(*caps));
        *error = MEGLER_CAMERA_FAILED;
        std::snprintf(caps->why, sizeof(caps->why), "Camera host has shut down");
        qWarning() << "camera:" << caps->why;
    }
    return result;
}

bool MEGLER_CB start(void *user, void *camera, int width, int height, int fps, megler_camera_frame_fn frame, void *core)
{
    bool result = false;
    static_cast<Worker *>(user)->call([&] { result = static_cast<Worker *>(user)->start(static_cast<Camera *>(camera), width, height, fps, frame, core); });
    if (!result) qWarning() << "camera: unsupported or failed start" << width << height << fps;
    return result;
}

void MEGLER_CB stop(void *user, void *camera)
{
    static_cast<Worker *>(user)->call([&] { static_cast<Worker *>(user)->stop(static_cast<Camera *>(camera)); });
}

bool MEGLER_CB focus(void *, void *, int mode, megler_camera_focus_fn, void *)
{
    if (mode == MEGLER_CAMERA_FOCUS_NONE) return true;
    qWarning() << "camera: focus is not supported";
    return false;
}

void MEGLER_CB torch(void *, void *, bool on)
{
    if (on) qWarning() << "camera: torch is not supported";
}

void MEGLER_CB close(void *user, void *camera)
{
    static_cast<Worker *>(user)->closeHandle(static_cast<Camera *>(camera));
}
}

megler_camera_host auroraCameraHost()
{
    megler_camera_host host = {};
    host.user = worker();
    host.count = count;
    host.info = info;
    host.open = open;
    host.start = start;
    host.stop = stop;
    host.focus = focus;
    host.torch = torch;
    host.close = close;
    return host;
}

void auroraCameraShutdown()
{
    worker()->shutdown();
}

#include "camera_host.moc"
