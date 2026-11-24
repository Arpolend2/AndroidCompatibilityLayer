// Build this file alone with Qt Core, Gui, Multimedia and camera_host.moc.
#define AURORA_CAMERA_TEST
#include "../camera_host.cpp"

#include <QCoreApplication>
#include <QBuffer>
#include <QPointer>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace {
struct Received {
    int pictures = 0;
    int stopped = 0;
    std::vector<uint8_t> data;
};

void MEGLER_CB receive(void *context, const megler_camera_frame *frame)
{
    Received *r = static_cast<Received *>(context);
    if (!frame) {
        ++r->stopped;
        return;
    }
    assert(frame->width == 2 && frame->height == 2);
    assert(frame->y_stride == 2 && frame->uv_stride == 2);
    r->data.assign(frame->y, frame->y + 4);
    r->data.insert(r->data.end(), frame->uv, frame->uv + 2);
    ++r->pictures;
}

void MEGLER_CB countFrame(void *context, const megler_camera_frame *frame)
{
    if (frame) ++*static_cast<int *>(context);
}

struct BlockingCallback {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    int calls = 0;
    int stopped = 0;
};

void MEGLER_CB receiveBlocking(void *context, const megler_camera_frame *frame)
{
    BlockingCallback *state = static_cast<BlockingCallback *>(context);
    std::unique_lock<std::mutex> lock(state->mutex);
    ++state->calls;
    if (!frame) ++state->stopped;
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [state] { return state->released; });
}

void MEGLER_CB focusResult(void *context, bool focused)
{
    int *result = static_cast<int *>(context);
    *result += focused ? 1 : 10;
}

struct JpegOutput {
    QByteArray bytes;
    bool accept = true;
};

bool MEGLER_CB collectJpeg(void *context, const uint8_t *bytes, size_t size)
{
    JpegOutput *output = static_cast<JpegOutput *>(context);
    if (!output->accept) return false;
    output->bytes.append(reinterpret_cast<const char *>(bytes), static_cast<int>(size));
    return true;
}

void testStillCaptureHelpers()
{
    assert(validPictureSize(QSize(4032, 3024)));
    assert(!validPictureSize(QSize(0, 480)));
    assert(!validPictureSize(QSize(16385, 100)));
    assert(!validPictureSize(QSize(10001, 10000)));
    assert(normalizePictureSizes({}).isEmpty());
    const QList<QSize> sizes = normalizePictureSizes(
        { QSize(1920, 1080), QSize(4032, 3024), QSize(4032, 3024), QSize(0, 720), QSize(1280, 720) });
    assert(sizes.size() == 3 && sizes[0] == QSize(4032, 3024) &&
           sizes[1] == QSize(1920, 1080) && sizes[2] == QSize(1280, 720));

    const QByteArray source = QByteArray::fromHex("ffd8ffd9");
    const int rotations[] = { 0, 90, 180, 270 };
    const int orientations[] = { 1, 6, 3, 8 };
    for (int i = 0; i < 4; ++i) {
        QBuffer file;
        file.setData(source);
        assert(file.open(QIODevice::ReadOnly));
        JpegOutput output;
        assert(forwardJpeg(&file, rotations[i], collectJpeg, &output));
        assert(output.bytes.size() == source.size() + 36);
        assert(output.bytes.startsWith(QByteArray::fromHex("ffd8ffe10022")));
        assert(output.bytes.mid(6, 6) == QByteArray("Exif\0\0", 6));
        assert(static_cast<uint8_t>(output.bytes[30]) == orientations[i]);
        assert(output.bytes.endsWith(QByteArray::fromHex("ffd9")));
    }

    QBuffer invalid;
    invalid.setData(QByteArray::fromHex("ffd9ffd8"));
    assert(invalid.open(QIODevice::ReadOnly));
    JpegOutput output;
    assert(!forwardJpeg(&invalid, 0, collectJpeg, &output));
    QBuffer invalidRotation;
    invalidRotation.setData(source);
    assert(invalidRotation.open(QIODevice::ReadOnly));
    assert(!forwardJpeg(&invalidRotation, 45, collectJpeg, &output));
    QBuffer refused;
    refused.setData(source);
    assert(refused.open(QIODevice::ReadOnly));
    output.accept = false;
    assert(!forwardJpeg(&refused, 0, collectJpeg, &output));
}

void testViewfinderStill()
{
    Surface surface;
    Received received;
    QVideoFrame frame(6, QSize(2, 2), 2, QVideoFrame::Format_NV12);
    assert(frame.map(QAbstractVideoBuffer::WriteOnly));
    const uint8_t bytes[] = {81, 81, 81, 81, 90, 240};
    std::memcpy(frame.bits(), bytes, sizeof(bytes));
    frame.unmap();
    surface.enable(receive, &received, 2, 2, QSize(2, 2));
    std::vector<uint8_t> snapshot;
    assert(!surface.latestSnapshot(QSize(2, 2), &snapshot));
    assert(surface.present(frame));
    assert(!surface.latestSnapshot(QSize(4, 2), &snapshot));
    assert(surface.latestSnapshot(QSize(2, 2), &snapshot));
    assert(snapshot == std::vector<uint8_t>(bytes, bytes + sizeof(bytes)));

    JpegOutput output;
    assert(encodeViewfinderJpeg(snapshot, QSize(2, 2), 95, 270, collectJpeg, &output));
    assert(output.bytes.startsWith(QByteArray::fromHex("ffd8ffe10022")));
    assert(static_cast<uint8_t>(output.bytes[30]) == 8);
    const QImage decoded = QImage::fromData(output.bytes, "JPEG");
    assert(decoded.size() == QSize(2, 2));
    const QColor color(decoded.pixel(0, 0));
    assert(color.red() > 200 && color.green() < 60 && color.blue() < 60);
    assert(!encodeViewfinderJpeg(snapshot, QSize(4, 2), 95, 0, collectJpeg, &output));
    assert(!encodeViewfinderJpeg(snapshot, QSize(2, 2), 0, 0, collectJpeg, &output));
    assert(!encodeViewfinderJpeg(snapshot, QSize(2, 2), 95, 45, collectJpeg, &output));
    output.accept = false;
    assert(!encodeViewfinderJpeg(snapshot, QSize(2, 2), 95, 0, collectJpeg, &output));
    Worker worker;
    Camera camera;
    camera.surface = &surface;
    camera.streaming = true;
    camera.viewfinderStillSize = QSize(2, 2);
    camera.pictureSizes.append(camera.viewfinderStillSize);
    worker.addTestCamera(&camera);
    JpegOutput captured;
    assert(worker.capture(&camera, 2, 2, 90, 90, collectJpeg, &captured));
    assert(static_cast<uint8_t>(captured.bytes[30]) == 6);
    assert(QImage::fromData(captured.bytes, "JPEG").size() == QSize(2, 2));
    assert(!worker.capture(&camera, 4, 2, 90, 90, collectJpeg, &captured));
    surface.disable();
    assert(!surface.latestSnapshot(QSize(2, 2), &snapshot));
    assert(!worker.capture(&camera, 2, 2, 90, 90, collectJpeg, &captured));

    const int width = 2592, height = 1944;
    QVideoFrame large(width * height * 3 / 2, QSize(width, height), width, QVideoFrame::Format_NV21);
    assert(large.map(QAbstractVideoBuffer::WriteOnly));
    std::memset(large.bits(), 100, width * height);
    uint8_t *vu = large.bits() + width * height;
    for (int i = 0; i < width * height / 2; i += 2) {
        vu[i] = 220;
        vu[i + 1] = 80;
    }
    large.unmap();
    int delivered = 0;
    surface.enable(countFrame, &delivered, width / 2, height / 2, QSize(width, height));
    assert(surface.present(large) && delivered == 1);
    assert(surface.latestSnapshot(QSize(width, height), &snapshot));
    assert(snapshot.size() == static_cast<size_t>(width) * height * 3 / 2);
    assert(snapshot[0] == 100 && snapshot[width * height] == 80 && snapshot[width * height + 1] == 220);
    JpegOutput fullResolution;
    assert(encodeViewfinderJpeg(snapshot, QSize(width, height), 90, 90, collectJpeg, &fullResolution));
    assert(QImage::fromData(fullResolution.bytes, "JPEG").size() == QSize(width, height));
    assert(static_cast<uint8_t>(fullResolution.bytes[30]) == 6);
    QVideoFrame changed(width * height * 3 / 8, QSize(width / 2, height / 2),
                        width / 2, QVideoFrame::Format_NV12);
    assert(changed.map(QAbstractVideoBuffer::WriteOnly));
    std::memset(changed.bits(), 128, width * height * 3 / 8);
    changed.unmap();
    assert(surface.present(changed) && delivered == 2);
    assert(!surface.latestSnapshot(QSize(width, height), &snapshot));
    surface.disable();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testStillCaptureHelpers();
    testViewfinderStill();
    Surface surface;
    Received r;
    QVideoFrame frame(6, QSize(2, 2), 2, QVideoFrame::Format_NV21);
    assert(frame.map(QAbstractVideoBuffer::WriteOnly));
    const uint8_t bytes[] = {1, 2, 3, 4, 6, 5};
    std::memcpy(frame.bits(), bytes, sizeof(bytes));
    frame.unmap();
    surface.enable(receive, &r, 2, 2);
    assert(surface.present(frame));
    assert(r.pictures == 1);
    assert((r.data == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    surface.disable();
    assert(surface.present(frame));
    assert(r.pictures == 1);
    surface.enable(receive, &r, 4, 2);
    assert(!surface.present(frame));
    assert(r.stopped == 1);
    assert(surface.present(frame));
    assert(r.stopped == 1);

    ProbeStats stats;
    stats.observe(QSize(2, 2), 0);
    stats.observe(QSize(2, 2), 100000);
    assert(!stats.observedFps());
    megler_camera_caps observed = {};
    assert(!capsFromProbe(stats, &observed) && observed.size_count == 0);
    stats.observe(QSize(2, 2), 200000);
    assert(stats.observedFps() == 10);
    assert(capsFromProbe(stats, &observed));
    assert(observed.size_count == 1 && observed.sizes[0].width == 2 &&
           observed.sizes[0].height == 2 && observed.sizes[0].min_fps == 10 &&
           observed.sizes[0].max_fps == 10);
    stats.observe(QSize(4, 2), 300000);
    assert(!stats.observedFps() && stats.frames == 1 && stats.size == QSize(4, 2));
    stats.observe(QSize(4, 2), 200000);
    assert(!stats.observedFps() && stats.frames == 1);
    stats.observe(QSize(3, 2), 300000);
    stats.observe(QSize(3, 2), 400000);
    stats.observe(QSize(3, 2), 500000);
    assert(!capsFromProbe(stats, &observed));

    assert(outputFactor(QSize(1280, 720)) == 1 && outputFactor(QSize(1920, 1080)) == 1);
    assert(outputFactor(QSize(3840, 2160)) == 2 && outputFactor(QSize(2592, 1944)) == 2);
    assert(outputFactor(QSize(3842, 2160)) == 0 && outputFactor(QSize(4096, 3072)) == 0);
    assert(outputFactor(QSize(2590, 1944)) == 0 && outputFactor(QSize(0, 0)) == 0);
    QList<QCameraViewfinderSettings> settings;
    megler_camera_caps selected = {};
    collectCaps(&settings, &selected);
    assert(selected.size_count == 0);
    QCameraViewfinderSettings unknownRate;
    unknownRate.setResolution(640, 480);
    unknownRate.setPixelFormat(QVideoFrame::Format_NV12);
    settings.append(unknownRate);
    collectCaps(&settings, &selected);
    assert(selected.size_count == 0);
    assert(std::strcmp(settingRejection(unknownRate), "missing or invalid frame rate") == 0);
    QCameraViewfinderSettings planar;
    planar.setResolution(640, 480);
    planar.setPixelFormat(QVideoFrame::Format_YUV420P);
    planar.setMinimumFrameRate(15);
    planar.setMaximumFrameRate(30);
    settings.append(planar);
    collectCaps(&settings, &selected);
    assert(selected.size_count == 1 && selected.sizes[0].width == 640 &&
           selected.sizes[0].min_fps == 15 && selected.sizes[0].max_fps == 30);

    QVideoFrame planarFrame(6, QSize(2, 2), 2, QVideoFrame::Format_YUV420P);
    assert(planarFrame.map(QAbstractVideoBuffer::WriteOnly));
    const uint8_t planarBytes[] = {1, 2, 3, 4, 5, 6};
    std::memcpy(planarFrame.bits(), planarBytes, sizeof(planarBytes));
    planarFrame.unmap();
    surface.enable(receive, &r, 2, 2);
    assert(surface.present(planarFrame));
    assert((r.data == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    surface.disable();
    QVideoFrame yvFrame(6, QSize(2, 2), 2, QVideoFrame::Format_YV12);
    assert(yvFrame.map(QAbstractVideoBuffer::WriteOnly));
    std::memcpy(yvFrame.bits(), bytes, sizeof(bytes));
    yvFrame.unmap();
    surface.enable(receive, &r, 2, 2);
    assert(surface.present(yvFrame));
    assert((r.data == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    surface.disable();
    surface.beginProbe();
    assert(surface.present(planarFrame));
    QThread::msleep(110);
    assert(surface.present(planarFrame));
    QThread::msleep(110);
    assert(surface.present(planarFrame));
    ProbeStats receivedProbe = surface.probeState(nullptr);
    megler_camera_caps measured = {};
    assert(capsFromProbe(receivedProbe, &measured));
    assert(measured.sizes[0].width == 2 && measured.sizes[0].height == 2 &&
           measured.sizes[0].min_fps > 0);
    surface.endProbe();
    surface.beginProbe();
    QVideoFrame rgb(16, QSize(2, 2), 8, QVideoFrame::Format_RGB32);
    assert(!surface.present(rgb));
    QString probeError;
    assert(!surface.probeState(&probeError).frames && !probeError.isEmpty());
    surface.endProbe();

    BlockingCallback blocking;
    surface.enable(receiveBlocking, &blocking, 2, 2);
    std::thread producer([&] { assert(surface.present(frame)); });
    {
        std::unique_lock<std::mutex> lock(blocking.mutex);
        blocking.cv.wait(lock, [&] { return blocking.entered; });
    }
    std::thread stopper([&] { surface.disable(); });
    {
        std::lock_guard<std::mutex> lock(blocking.mutex);
        blocking.released = true;
        blocking.cv.notify_all();
    }
    producer.join();
    stopper.join();
    assert(surface.present(frame));
    assert(blocking.calls == 1);

    megler_camera_host host = auroraCameraHost();
    megler_camera_info infoResult = {};
    megler_camera_caps caps = {};
    int error = MEGLER_CAMERA_OK;
    assert(!host.info(host.user, -1, &infoResult));
    assert(!host.open(host.user, -1, &caps, &error));
    assert(error == MEGLER_CAMERA_FAILED && caps.why[0]);
    assert(!host.focus(host.user, nullptr, MEGLER_CAMERA_FOCUS_ONCE, nullptr, nullptr));
    assert(host.focus(host.user, nullptr, MEGLER_CAMERA_FOCUS_NONE, nullptr, nullptr));

    Worker *cameraWorker = static_cast<Worker *>(host.user);
    Camera *camera = nullptr;
    QPointer<QCamera> device;
    QPointer<Surface> cameraSurface;
    BlockingCallback pending;
    assert(cameraWorker->call([&] {
        camera = new Camera;
        camera->device = new QCamera;
        camera->surface = new Surface;
        camera->surface->enable(receiveBlocking, &pending, 2, 2);
        device = camera->device;
        cameraSurface = camera->surface;
        cameraWorker->addTestCamera(camera);
    }));
    int focused = 0;
    assert(!host.focus(host.user, camera, MEGLER_CAMERA_FOCUS_ONCE, focusResult, &focused));
    assert(cameraWorker->call([&] {
        camera->focusDone = focusResult;
        camera->focusCore = &focused;
        cameraWorker->testCompleteFocus(camera, true);
        cameraWorker->testCompleteFocus(camera, false);
    }));
    assert(focused == 1);
    assert(cameraWorker->call([&] {
        camera->focusDone = focusResult;
        camera->focusCore = &focused;
    }));
    assert(host.focus(host.user, camera, MEGLER_CAMERA_FOCUS_NONE, nullptr, nullptr));
    assert(cameraWorker->call([&] { cameraWorker->testCompleteFocus(camera, true); }));
    assert(focused == 1);
    Surface *surfacePointer = cameraSurface.data();
    std::thread delivering([&] { assert(surfacePointer->present(frame)); });
    {
        std::unique_lock<std::mutex> lock(pending.mutex);
        pending.cv.wait(lock, [&] { return pending.entered; });
    }

    std::mutex shutdownMutex;
    std::condition_variable shutdownCv;
    bool shutdownStarted = false, shutdownFinished = false;
    std::thread shutdown([&] {
        {
            std::lock_guard<std::mutex> lock(shutdownMutex);
            shutdownStarted = true;
            shutdownCv.notify_all();
        }
        auroraCameraShutdown();
        {
            std::lock_guard<std::mutex> lock(shutdownMutex);
            shutdownFinished = true;
            shutdownCv.notify_all();
        }
    });
    {
        std::unique_lock<std::mutex> lock(shutdownMutex);
        shutdownCv.wait(lock, [&] { return shutdownStarted; });
        assert(!shutdownCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return shutdownFinished; }));
    }
    megler_camera_caps lateCaps = {};
    int lateError = MEGLER_CAMERA_OK;
    void *lateCamera = nullptr;
    std::thread lateOpen([&] { lateCamera = host.open(host.user, 0, &lateCaps, &lateError); });
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        pending.released = true;
        pending.cv.notify_all();
    }
    delivering.join();
    shutdown.join();
    lateOpen.join();
    assert(shutdownFinished && device.isNull() && cameraSurface.isNull());
    assert(pending.calls == 2 && pending.stopped == 1);
    assert(!lateCamera && lateError == MEGLER_CAMERA_FAILED &&
           std::strstr(lateCaps.why, "shut down"));
    assert(!cameraWorker->thread()->isRunning());
    assert(host.count(host.user) == 0);
    assert(!host.info(host.user, 0, &infoResult));
    std::memset(&caps, 0, sizeof(caps));
    error = MEGLER_CAMERA_OK;
    assert(!host.open(host.user, 0, &caps, &error));
    assert(error == MEGLER_CAMERA_FAILED && std::strstr(caps.why, "shut down"));
    assert(!host.start(host.user, camera, 2, 2, 30, receive, &r));
    host.stop(host.user, camera);
    host.close(host.user, camera);
    auroraCameraShutdown();
}
