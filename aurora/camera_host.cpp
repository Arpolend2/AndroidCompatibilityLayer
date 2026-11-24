#include "camera_host.h"
#include "camera_qt_frame.h"

#include <QAbstractVideoSurface>
#include <QCamera>
#include <QCameraExposure>
#include <QCameraFocus>
#include <QCameraImageCapture>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QBuffer>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageEncoderSettings>
#include <QImageWriter>
#include <QIODevice>
#include <QScreen>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QTemporaryDir>
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

    QSize nativeSize()
    {
        QMutexLocker lock(&mutex);
        return lastNativeSize;
    }

    void enable(megler_camera_frame_fn fn, void *core, int width, int height, const QSize &stillSize = QSize())
    {
        QMutexLocker lock(&mutex);
        callback = fn;
        callbackCore = core;
        expected = QSize(width, height);
        snapshotSize = stillSize;
        snapshot.clear();
        nextSnapshotMs = 0;
        clock.start();
    }

    void disable()
    {
        QMutexLocker lock(&mutex);
        callback = nullptr;
        callbackCore = nullptr;
        snapshot.clear();
        snapshotSize = QSize();
    }

    bool latestSnapshot(const QSize &size, std::vector<uint8_t> *out)
    {
        QMutexLocker lock(&mutex);
        if (!out || size != snapshotSize || snapshot.empty()) return false;
        *out = snapshot;
        return true;
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
        // Some backends deliver empty placeholder frames first; wait for a real one.
        if (!frame.isValid() || frame.size().isEmpty() || frame.pixelFormat() == QVideoFrame::Format_Invalid) {
            if (++emptyFrames == 1 || emptyFrames % 100 == 0)
                qWarning() << "camera: ignoring an empty viewfinder frame, count" << emptyFrames;
            return true;
        }
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
        lastNativeSize = frame.size();
        if (probing) {
            probe.observe(outSize, clock.nsecsElapsed() / 1000);
            frame.unmap();
            return true;
        }
        if (!snapshotSize.isEmpty()) {
            if (snapshotSize != frame.size()) {
                if (!snapshot.empty())
                    qWarning() << "camera: native viewfinder size changed; discarding the still frame";
                snapshot.clear();
            } else if (clock.elapsed() >= nextSnapshotMs) {
                if (factor == 1) snapshot = pixels;
                else if (!aurora_camera::copyMappedFrame(frame, &snapshot, 1)) {
                    snapshot.clear();
                    qWarning() << "camera: could not copy the full-size viewfinder frame for a still";
                }
                nextSnapshotMs = clock.elapsed() + 250;
            }
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
    std::vector<uint8_t> snapshot;
    QSize snapshotSize;
    QSize lastNativeSize;
    qint64 nextSnapshotMs = 0;
    ProbeStats probe;
    QString probeError;
    bool probing = false;
    int emptyFrames = 0;
};

struct Camera {
    QCamera *device = nullptr;
    Surface *surface = nullptr;
    QCameraImageCapture *imageCapture = nullptr;
    QString name;
    QString jpegCodec;
    QList<QCameraViewfinderSettings> settings;
    QList<QSize> pictureSizes;
    megler_camera_caps caps = {};
    bool streaming = false;
    bool observedDefault = false;
    bool stillSwitchFailed = false;
    QSize viewfinderStillSize;
    megler_camera_focus_fn focusDone = nullptr;
    void *focusCore = nullptr;
    int focusMode = MEGLER_CAMERA_FOCUS_NONE;
    QTimer *focusTimer = nullptr;
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
    int fps = stats.observedFps();
    if (!fps || !validSize(stats.size)) return false;
    // Measured rates jitter (31 for a 30 fps sensor); apps pick from standard ranges.
    if (fps >= 28 && fps <= 32) fps = 30;
    else if (fps >= 14 && fps <= 16) fps = 15;
    else if (fps >= 23 && fps <= 26) fps = 24;
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

bool validPictureSize(const QSize &size)
{
    return size.width() > 0 && size.height() > 0 && size.width() <= 16384 && size.height() <= 16384 &&
           static_cast<qint64>(size.width()) * size.height() <= 100000000;
}

QList<QSize> normalizePictureSizes(const QList<QSize> &reported)
{
    QList<QSize> sizes;
    for (const QSize &size : reported)
        if (validPictureSize(size) && !sizes.contains(size)) sizes.append(size);
    std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
        const qint64 areaA = static_cast<qint64>(a.width()) * a.height();
        const qint64 areaB = static_cast<qint64>(b.width()) * b.height();
        return areaA == areaB ? a.width() > b.width() : areaA > areaB;
    });
    while (sizes.size() > MEGLER_CAMERA_MAX_SIZES) sizes.removeLast();
    return sizes;
}

QList<QSize> collectPictureSizes(QCameraImageCapture *capture, QString *codec)
{
    QList<QSize> sizes;
    if (!capture || !capture->isAvailable()) return sizes;
    const QStringList codecs = capture->supportedImageCodecs();
    for (const QString &available : codecs) {
        const QString normalized = available.toLower();
        if (normalized == QStringLiteral("jpeg") || normalized == QStringLiteral("jpg")) {
            *codec = available;
            break;
        }
    }
    if (codec->isEmpty()) {
        qWarning() << "camera: still capture has no supported JPEG codec";
        return sizes;
    }
    QImageEncoderSettings encoding;
    encoding.setCodec(*codec);
    bool continuous = false;
    const QList<QSize> reported = capture->supportedResolutions(encoding, &continuous);
    if (continuous)
        qWarning() << "camera: backend reports continuous still resolutions; only its explicit sizes are advertised";
    for (const QSize &size : reported)
        qWarning() << "camera: Qt still resolution" << size << (validPictureSize(size) ? "usable" : "rejected");
    sizes = normalizePictureSizes(reported);
    return sizes;
}

bool forwardJpeg(QIODevice *file, int rotation, megler_camera_jpeg_write_fn write, void *context)
{
    uint8_t exif[36] = {
        0xff, 0xe1, 0x00, 0x22,
        'E', 'x', 'i', 'f', 0x00, 0x00,
        'I', 'I', 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    const qint64 size = file->size();
    const qint64 added = static_cast<qint64>(sizeof(exif));
    char start[2], end[2], buffer[65536];
    if (!write || !file->isOpen() || size < 4 ||
        size > static_cast<qint64>(MEGLER_CAMERA_MAX_JPEG_BYTES) - added ||
        !file->seek(0) || file->read(start, sizeof(start)) != sizeof(start) ||
        !file->seek(size - 2) || file->read(end, sizeof(end)) != sizeof(end) ||
        static_cast<uint8_t>(start[0]) != 0xff || static_cast<uint8_t>(start[1]) != 0xd8 ||
        static_cast<uint8_t>(end[0]) != 0xff || static_cast<uint8_t>(end[1]) != 0xd9)
        return false;
    switch (rotation) {
    case 0: exif[28] = 1; break;
    case 90: exif[28] = 6; break;
    case 180: exif[28] = 3; break;
    case 270: exif[28] = 8; break;
    default: return false;
    }
    if (!file->seek(0) || file->read(start, sizeof(start)) != sizeof(start) ||
        !write(context, reinterpret_cast<const uint8_t *>(start), sizeof(start)) ||
        !write(context, exif, sizeof(exif)) || !file->seek(2))
        return false;
    for (;;) {
        const qint64 read = file->read(buffer, sizeof(buffer));
        if (read < 0) return false;
        if (!read) break;
        if (!write(context, reinterpret_cast<const uint8_t *>(buffer), static_cast<size_t>(read))) return false;
    }
    return file->atEnd();
}

bool encodeViewfinderJpeg(const std::vector<uint8_t> &nv12, const QSize &size, int quality, int rotation,
                          megler_camera_jpeg_write_fn write, void *context)
{
    const int width = size.width(), height = size.height();
    if (outputFactor(size) == 0 || quality < 1 || quality > 100 ||
        nv12.size() != static_cast<size_t>(width) * height * 3 / 2)
        return false;
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) return false;
    const size_t chroma = static_cast<size_t>(width) * height;
    for (int y = 0; y < height; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t pair = chroma + static_cast<size_t>(y / 2) * width + (x & ~1);
            const int u = nv12[pair] - 128, v = nv12[pair + 1] - 128;
            const int l = 298 * (nv12[static_cast<size_t>(y) * width + x] - 16) + 128;
            row[x] = qRgb(qBound(0, (l + 409 * v) >> 8, 255),
                          qBound(0, (l - 100 * u - 208 * v) >> 8, 255),
                          qBound(0, (l + 516 * u) >> 8, 255));
        }
    }
    QBuffer jpeg;
    if (!jpeg.open(QIODevice::ReadWrite)) return false;
    QImageWriter writer(&jpeg, "jpeg");
    writer.setQuality(quality);
    if (!writer.write(image)) {
        qWarning() << "camera: viewfinder JPEG encoding failed:" << writer.errorString();
        return false;
    }
    return forwardJpeg(&jpeg, rotation, write, context);
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

    QHash<QString, megler_camera_caps> defaultCaps;
    QHash<QString, QSize> defaultNativeSizes;

    bool waitFor(QCamera *camera, QCamera::Status wanted, int timeoutMs = 10000)
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
        timer.start(timeoutMs);
        loop.exec();
        return camera->status() == wanted && camera->error() == QCamera::NoError;
    }

    // Reads the still capabilities without touching the pipeline: no capture-mode switch, no start/stop.
    // Switching a Rockchip camera between viewfinder and 4K still has wedged the system 3A (ISP) server.
    void probeStillCapture(Camera *camera)
    {
        camera->imageCapture = new QCameraImageCapture(camera->device, camera->device);
        camera->pictureSizes = collectPictureSizes(camera->imageCapture, &camera->jpegCodec);
        qWarning() << "camera: usable JPEG still sizes (read without starting the camera):" << camera->pictureSizes.size();
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
        defaultNativeSizes.insert(c->name, c->surface->nativeSize());
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
            unloadDevice(c->device);
            delete c->surface;
            delete c;
            return nullptr;
        }
        probeStillCapture(c);
        c->settings = c->device->supportedViewfinderSettings();
        qWarning() << "camera: Qt reported" << c->settings.size() << "viewfinder settings";
        for (const QCameraViewfinderSettings &s : c->settings) {
            qWarning() << "camera: Qt setting" << s.resolution() << "format" << int(s.pixelFormat())
                       << "fps" << s.minimumFrameRate() << s.maximumFrameRate()
                       << (settingRejection(s) ? settingRejection(s) : "usable");
        }
        collectCaps(&c->settings, caps);
        qWarning() << "camera: usable reported modes:" << caps->size_count;
        if (!caps->size_count) {
            const auto known = defaultCaps.constFind(c->name);
            if (known != defaultCaps.constEnd()) {
                *caps = *known;
                c->observedDefault = true;
                qWarning() << "camera: reusing the cached default viewfinder probe";
            } else if (!probeDefault(c, caps)) {
                unloadDevice(c->device);
                delete c->surface;
                delete c;
                return nullptr;
            } else {
                defaultCaps.insert(c->name, *caps);
            }
        }
        if (info.position() == QCamera::FrontFace &&
            info.description() == QStringLiteral("Rockchip front camera") && c->observedDefault) {
            const QSize native = defaultNativeSizes.value(c->name);
            c->pictureSizes.clear();
            if (outputFactor(native) && QImageWriter::supportedImageFormats().contains("jpeg")) {
                c->viewfinderStillSize = native;
                c->pictureSizes.append(native);
                qWarning() << "camera: Rockchip front stills use viewfinder JPEG at" << native;
            } else {
                qWarning() << "camera: Rockchip front viewfinder JPEG unavailable, native size" << native;
            }
        }
        QCameraFocus *f = c->device->focus();
        QCameraExposure *e = c->device->exposure();
        caps->focus = f && c->device->supportedLocks().testFlag(QCamera::LockFocus) &&
                      f->isFocusModeSupported(QCameraFocus::AutoFocus) &&
                      f->isFocusModeSupported(QCameraFocus::ContinuousFocus);
        caps->macro = caps->focus && f->isFocusModeSupported(QCameraFocus::MacroFocus);
        caps->torch = e && e->isFlashModeSupported(QCameraExposure::FlashTorch) &&
                      e->isFlashModeSupported(QCameraExposure::FlashOff);
        qWarning() << "camera: supported focus" << caps->focus << "macro" << caps->macro
                   << "torch" << caps->torch;
        c->caps = *caps;
        c->focusTimer = new QTimer(c->device);
        c->focusTimer->setSingleShot(true);
        connect(c->focusTimer, &QTimer::timeout, c->device, [c] {
            qWarning() << "camera: focus timed out";
            completeFocus(c, false);
        });
        connect(c->device, static_cast<void (QCamera::*)(QCamera::LockType, QCamera::LockStatus,
                                                          QCamera::LockChangeReason)>(&QCamera::lockStatusChanged),
                c->device, [c](QCamera::LockType type, QCamera::LockStatus status, QCamera::LockChangeReason reason) {
            if (type != QCamera::LockFocus || !c->focusDone) return;
            if (status == QCamera::Locked) completeFocus(c, true);
            else if (status == QCamera::Unlocked && reason == QCamera::LockFailed) completeFocus(c, false);
        });
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
        c->surface->enable(frame, core, width, height, c->viewfinderStillSize);
        c->streaming = true;
        return true;
    }

    void stop(Camera *c)
    {
        if (!c) return;
        cancelFocus(c);
        if (c->caps.torch) c->device->exposure()->setFlashMode(QCameraExposure::FlashOff);
        c->surface->disable();
        c->device->stop();
        c->streaming = false;
    }

    bool focus(Camera *c, int mode, megler_camera_focus_fn done, void *core)
    {
        if (!c) return mode == MEGLER_CAMERA_FOCUS_NONE;
        if (!cameras.contains(c)) return false;
        cancelFocus(c);
        if (mode == MEGLER_CAMERA_FOCUS_NONE) return true;
        if (!c->streaming || !c->caps.focus || !done ||
            (mode == MEGLER_CAMERA_FOCUS_MACRO && !c->caps.macro)) return false;
        QCameraFocus::FocusModes qtMode;
        switch (mode) {
        case MEGLER_CAMERA_FOCUS_ONCE: qtMode = QCameraFocus::AutoFocus; break;
        case MEGLER_CAMERA_FOCUS_MACRO: qtMode = QCameraFocus::MacroFocus; break;
        case MEGLER_CAMERA_FOCUS_CONTINUOUS: qtMode = QCameraFocus::ContinuousFocus; break;
        default: return false;
        }
        c->focusMode = mode;
        c->focusDone = done;
        c->focusCore = core;
        c->device->focus()->setFocusMode(qtMode);
        c->focusTimer->start(10000);
        c->device->searchAndLock(QCamera::LockFocus);
        return true;
    }

    void torch(Camera *c, bool on)
    {
        if (!c || !cameras.contains(c) || !c->streaming || !c->caps.torch) {
            if (on) qWarning() << "camera: torch unavailable";
            return;
        }
        c->device->exposure()->setFlashMode(on ? QCameraExposure::FlashTorch : QCameraExposure::FlashOff);
    }

    int pictureSizes(Camera *c, megler_camera_picture_size *sizes, int max)
    {
        if (!c || !cameras.contains(c) || !sizes || max <= 0) return 0;
        const int count = std::min(max, c->pictureSizes.size());
        for (int i = 0; i < count; ++i) {
            sizes[i].width = c->pictureSizes[i].width();
            sizes[i].height = c->pictureSizes[i].height();
        }
        return count;
    }

    bool capture(Camera *c, int width, int height, int quality, int rotation,
                 megler_camera_jpeg_write_fn write, void *context)
    {
        if (!c || !cameras.contains(c) || !c->streaming ||
            !validPictureSize(QSize(width, height)) || quality < 1 || quality > 100)
            return false;
        if (!c->pictureSizes.contains(QSize(width, height))) {
            qWarning() << "camera: still size is not in the backend's reported capabilities" << width << height;
            return false;
        }
        qWarning() << "camera: still requested" << QSize(width, height) << "quality" << quality
                   << "rotation" << rotation;
        if (!c->viewfinderStillSize.isEmpty()) {
            std::vector<uint8_t> frame;
            if (!c->surface->latestSnapshot(QSize(width, height), &frame) ||
                !encodeViewfinderJpeg(frame, QSize(width, height), quality, rotation, write, context)) {
                qWarning() << "camera: viewfinder still could not be encoded";
                return false;
            }
            qWarning() << "camera: viewfinder still JPEG captured" << QSize(width, height);
            return true;
        }
        if (!c->imageCapture || c->jpegCodec.isEmpty()) return false;
        bool switched = false;
        if (!c->imageCapture->isReadyForCapture()) {
            if (c->stillSwitchFailed) {
                qWarning() << "camera: still mode switch failed earlier; not retrying";
                return false;
            }
            // One controlled switch per shot, only on user request: stop, still mode, start.
            if (!switchMode(c, QCamera::CaptureStillImage)) {
                c->stillSwitchFailed = true;
                qWarning() << "camera: could not enter still mode; restoring viewfinder once";
                switchMode(c, QCamera::CaptureViewfinder);
                return false;
            }
            switched = true;
            if (!c->imageCapture->isReadyForCapture()) {
                QEventLoop wait;
                QTimer limit;
                limit.setSingleShot(true);
                connect(c->imageCapture, &QCameraImageCapture::readyForCaptureChanged, &wait, [&](bool r) { if (r) wait.quit(); });
                connect(&limit, &QTimer::timeout, &wait, &QEventLoop::quit);
                limit.start(3000);
                wait.exec();
            }
            if (!c->imageCapture->isReadyForCapture()) {
                qWarning() << "camera: still capture is not ready after the mode switch";
                c->stillSwitchFailed = true;
                switchMode(c, QCamera::CaptureViewfinder);
                return false;
            }
        }
        const bool ok = doCapture(c, width, height, quality, rotation, write, context);
        if (switched && !switchMode(c, QCamera::CaptureViewfinder)) {
            qWarning() << "camera: could not return to viewfinder mode";
            c->stillSwitchFailed = true;
        }
        return ok;
    }

    bool switchMode(Camera *c, QCamera::CaptureModes mode)
    {
        c->device->stop();
        if (!waitFor(c->device, QCamera::LoadedStatus, 5000)) return false;
        c->device->setCaptureMode(mode);
        c->device->start();
        if (!waitFor(c->device, QCamera::ActiveStatus, 8000)) {
            qWarning() << "camera: start after mode switch failed:" << c->device->errorString();
            c->device->stop();
            return false;
        }
        return true;
    }

    bool doCapture(Camera *c, int width, int height, int quality, int rotation,
                   megler_camera_jpeg_write_fn write, void *context)
    {

        QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/megler-camera-XXXXXX"));
        if (!temporary.isValid()) {
            qWarning() << "camera: could not create private still-capture storage";
            return false;
        }
        const QString outputPath = QDir(temporary.path()).filePath(QStringLiteral("capture.jpg"));
        QImageEncoderSettings encoding;
        encoding.setCodec(c->jpegCodec);
        encoding.setResolution(QSize(width, height));
        const QMultimedia::EncodingQuality encodingQuality =
            quality <= 20 ? QMultimedia::VeryLowQuality :
            quality <= 40 ? QMultimedia::LowQuality :
            quality <= 60 ? QMultimedia::NormalQuality :
            quality <= 80 ? QMultimedia::HighQuality : QMultimedia::VeryHighQuality;
        encoding.setQuality(encodingQuality);
        c->imageCapture->setEncodingSettings(encoding);
        c->imageCapture->setCaptureDestination(QCameraImageCapture::CaptureToFile);

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool done = false, saved = false, timedOut = false, pendingSaved = false;
        int captureId = -1, pendingId = -1;
        QString savedPath, errorText, pendingPath, pendingError;
        QMetaObject::Connection savedConnection = connect(
            c->imageCapture, &QCameraImageCapture::imageSaved, &loop,
            [&](int id, const QString &path) {
                if (captureId < 0) {
                    pendingId = id;
                    pendingPath = path;
                    pendingSaved = true;
                } else if (id == captureId) {
                    saved = true;
                    savedPath = path;
                    done = true;
                    loop.quit();
                }
            });
        QMetaObject::Connection errorConnection = connect(
            c->imageCapture,
            static_cast<void (QCameraImageCapture::*)(int, QCameraImageCapture::Error, const QString &)>(&QCameraImageCapture::error),
            &loop, [&](int id, QCameraImageCapture::Error, const QString &message) {
                if (captureId < 0) {
                    pendingId = id;
                    pendingError = message;
                } else if (id == captureId) {
                    errorText = message;
                    done = true;
                    loop.quit();
                }
            });
        connect(&timeout, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            loop.quit();
        });
        timeout.start(20000);
        captureId = c->imageCapture->capture(outputPath);
        if (captureId >= 0 && pendingId == captureId) {
            if (pendingSaved) {
                saved = true;
                savedPath = pendingPath;
            } else {
                errorText = pendingError;
            }
            done = true;
        }
        if (captureId < 0) {
            qWarning() << "camera: backend refused still capture:" << c->imageCapture->errorString();
        } else if (!done) {
            loop.exec();
        }
        timeout.stop();
        if (timedOut) c->imageCapture->cancelCapture();
        disconnect(savedConnection);
        disconnect(errorConnection);
        if (timedOut) {
            qWarning() << "camera: still capture timed out";
            return false;
        }
        if (!saved) {
            qWarning() << "camera: still capture failed:" << errorText;
            return false;
        }
        const QFileInfo expectedInfo(outputPath), savedInfo(savedPath);
        if (savedInfo.canonicalFilePath().isEmpty() ||
            savedInfo.canonicalFilePath() != expectedInfo.canonicalFilePath()) {
            qWarning() << "camera: backend saved still image outside its private capture file";
            return false;
        }
        QFile file(outputPath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "camera: captured JPEG could not be opened:" << file.errorString();
            return false;
        }
        if (!forwardJpeg(&file, rotation, write, context)) {
            qWarning() << "camera: captured JPEG failed validation or exceeded the output limit";
            return false;
        }
        return true;
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
    void testCompleteFocus(Camera *c, bool success) { completeFocus(c, success); }
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
    static void completeFocus(Camera *c, bool success)
    {
        megler_camera_focus_fn done = c->focusDone;
        void *core = c->focusCore;
        const bool continuous = c->focusMode == MEGLER_CAMERA_FOCUS_CONTINUOUS;
        c->focusDone = nullptr;
        c->focusCore = nullptr;
        if (c->focusTimer) c->focusTimer->stop();
        if (continuous) c->device->unlock(QCamera::LockFocus);
        if (done) done(core, success);
    }

    static void cancelFocus(Camera *c)
    {
        c->focusDone = nullptr;
        c->focusCore = nullptr;
        c->focusMode = MEGLER_CAMERA_FOCUS_NONE;
        if (c->focusTimer) c->focusTimer->stop();
        if (c->device) c->device->unlock(QCamera::LockFocus);
    }

    // Stops and unloads the camera so the system pipeline is released cleanly before the object goes away.
    void unloadDevice(QCamera *device)
    {
        if (!device) return;
        device->stop();
        if (device->status() != QCamera::LoadedStatus && device->status() != QCamera::UnloadedStatus)
            waitFor(device, QCamera::LoadedStatus, 3000);
        device->unload();
        waitFor(device, QCamera::UnloadedStatus, 3000);
        delete device;
    }

    void release(Camera *c)
    {
        stop(c);
        unloadDevice(c->device);
        c->device = nullptr;
        c->surface->disable();
        delete c->surface;
        c->surface = nullptr;
        c->settings.clear();
        c->pictureSizes.clear();
        c->jpegCodec.clear();
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

int MEGLER_CB pictureSizes(void *user, void *camera, megler_camera_picture_size *sizes, int max)
{
    int result = 0;
    if (!static_cast<Worker *>(user)->call([&] {
            result = static_cast<Worker *>(user)->pictureSizes(static_cast<Camera *>(camera), sizes, max);
        }))
        return 0;
    return result;
}

bool MEGLER_CB capture(void *user, void *camera, int width, int height, int quality, int rotation,
                       megler_camera_jpeg_write_fn write, void *context)
{
    bool result = false;
    if (!static_cast<Worker *>(user)->call([&] {
            result = static_cast<Worker *>(user)->capture(static_cast<Camera *>(camera), width, height, quality,
                                                           rotation, write, context);
        }))
        return false;
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

bool MEGLER_CB focus(void *user, void *camera, int mode, megler_camera_focus_fn done, void *core)
{
    bool result = false;
    static_cast<Worker *>(user)->call([&] { result = static_cast<Worker *>(user)->focus(static_cast<Camera *>(camera), mode, done, core); });
    if (!result && mode != MEGLER_CAMERA_FOCUS_NONE) qWarning() << "camera: focus unavailable or invalid mode" << mode;
    return result;
}

void MEGLER_CB torch(void *user, void *camera, bool on)
{
    static_cast<Worker *>(user)->call([&] { static_cast<Worker *>(user)->torch(static_cast<Camera *>(camera), on); });
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

megler_camera_capture_host auroraCameraCaptureHost()
{
    megler_camera_capture_host host = {};
    host.user = worker();
    host.picture_sizes = pictureSizes;
    host.capture = capture;
    return host;
}

void auroraCameraShutdown()
{
    worker()->shutdown();
}

#include "camera_host.moc"
