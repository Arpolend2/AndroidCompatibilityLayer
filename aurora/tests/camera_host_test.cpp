// Build this file alone with Qt Core, Gui, Multimedia and camera_host.moc.
#define AURORA_CAMERA_TEST
#include "../camera_host.cpp"

#include <QCoreApplication>
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
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
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
