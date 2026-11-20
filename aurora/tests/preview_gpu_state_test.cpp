#include "../preview_gpu.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <thread>

class FrameNotifier : public QObject
{
    Q_OBJECT
public:
    PreviewGpuItem *item = nullptr;
    std::atomic<bool> expectFrame{false};
    int requests = 0;

public slots:
    void requestGpuFrame()
    {
        if (requests++ == 0)
            expectFrame.store(true);
        item->update();
    }
};

static bool waitUntil(const std::atomic<bool> &done, int timeout)
{
    QElapsedTimer timer;
    timer.start();
    while (!done.load() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done.load();
}

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.setGeometry(0, 0, 64, 64);
    std::shared_ptr<PreviewGpuState> state = std::make_shared<PreviewGpuState>();
    PreviewGpuItem *item = new PreviewGpuItem(state, window.contentItem());
    item->setSize(QSizeF(64, 64));
    FrameNotifier notifier;
    notifier.item = item;
    std::atomic<bool> producerDone{false}, produced{false}, observed{false}, early{false};
    std::atomic<bool> releaseRequested{false}, releaseDone{false}, released{false};
    std::atomic<bool> nextRequested{false}, nextDone{false}, nextOk{false};
    std::atomic<bool> resizeRequested{false}, resizeDone{false}, resizeOk{false};
    std::atomic<qint64> renderWaitMs{-1};
    QObject::connect(&window, &QQuickWindow::beforeRendering, &window, [&]() {
        if (!notifier.expectFrame.exchange(false))
            return;
        QElapsedTimer timer;
        timer.start();
        while (!producerDone.load() && timer.elapsed() < 500)
            QThread::msleep(1);
        renderWaitMs.store(timer.elapsed());
        early.store(producerDone.load() && produced.load());
        observed.store(true);
    }, Qt::DirectConnection);
    window.show();
    item->update();
    QElapsedTimer timer;
    timer.start();
    while (!state->qtContext() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QOpenGLContext *share = state->qtContext();
    if (!share || !share->isOpenGLES()) {
        std::fprintf(stderr, "SKIP: Qt Quick has no OpenGL ES scene graph\n");
        return 77;
    }

    QOffscreenSurface surface;
    surface.setFormat(share->format());
    surface.create();
    if (!surface.isValid()) {
        std::fprintf(stderr, "SKIP: offscreen producer surface unavailable\n");
        return 77;
    }
    std::thread producer([&]() {
        QOpenGLContext context;
        context.setFormat(share->format());
        context.setShareContext(share);
        if (!context.create() || !context.makeCurrent(&surface) || context.shareGroup() != share->shareGroup()) {
            producerDone.store(true);
            nextDone.store(true);
            resizeDone.store(true);
            releaseDone.store(true);
            return;
        }
        QOpenGLFunctions *gl = context.functions();
        gl->initializeOpenGLFunctions();
        GLuint texture = 0;
        gl->glGenTextures(1, &texture);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const unsigned char red[] = {255, 0, 0, 255};
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        gl->glFinish();
        const bool ok = texture && gl->glGetError() == GL_NO_ERROR &&
                        state->presentCanvas(texture, 1, 1, &notifier);
        if (ok) {
            const unsigned char green[] = {0, 255, 0, 255};
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
            gl->glFinish();
        }
        produced.store(ok && gl->glGetError() == GL_NO_ERROR);
        producerDone.store(true);
        while (!nextRequested.load())
            QThread::msleep(1);
        const bool second = ok && state->presentCanvas(texture, 1, 1, &notifier);
        if (second) {
            const unsigned char blue[] = {0, 0, 255, 255, 0, 0, 255, 255};
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
            gl->glFinish();
        }
        nextOk.store(second && gl->glGetError() == GL_NO_ERROR);
        nextDone.store(true);
        while (!resizeRequested.load())
            QThread::msleep(1);
        const bool resized = second && state->presentCanvas(texture, 2, 1, &notifier);
        if (resized) {
            const unsigned char yellow[] = {255, 255, 0, 255, 255, 255, 0, 255};
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
            gl->glFinish();
        }
        resizeOk.store(resized && gl->glGetError() == GL_NO_ERROR);
        resizeDone.store(true);
        while (!releaseRequested.load())
            QThread::msleep(1);
        const bool safeToDelete = !ok || state->releaseCanvasTexture(texture, &notifier);
        released.store(safeToDelete);
        if (safeToDelete)
            gl->glDeleteTextures(1, &texture);
        releaseDone.store(true);
        context.doneCurrent();
    });

    const bool frameObserved = waitUntil(observed, 3000);
    const bool producerFinished = waitUntil(producerDone, 2000);
    const QImage image = window.grabWindow();
    const QColor center = image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
    nextRequested.store(true);
    const bool nextFinished = waitUntil(nextDone, 2000);
    const QImage nextImage = window.grabWindow();
    const QColor nextCenter = nextImage.isNull() ? QColor() : nextImage.pixelColor(nextImage.width() / 2, nextImage.height() / 2);
    resizeRequested.store(true);
    const bool resizeFinished = waitUntil(resizeDone, 2000);
    const QImage resizedImage = window.grabWindow();
    const QColor resizedCenter = resizedImage.isNull() ? QColor() : resizedImage.pixelColor(resizedImage.width() / 2, resizedImage.height() / 2);
    releaseRequested.store(true);
    const bool releaseFinished = waitUntil(releaseDone, 2000);
    producer.join();
    if (!frameObserved || !producerFinished || !early.load() || !produced.load() ||
        !nextFinished || !nextOk.load() || !resizeFinished || !resizeOk.load() || !releaseFinished || !released.load() ||
        !center.isValid() || center.red() < 240 || center.green() > 15 || center.blue() > 15 ||
        !nextCenter.isValid() || nextCenter.red() > 15 || nextCenter.green() < 240 || nextCenter.blue() > 15 ||
        !resizedCenter.isValid() || resizedCenter.red() > 15 || resizedCenter.green() > 15 || resizedCenter.blue() < 240) {
        std::fprintf(stderr, "FAIL: frame must be independent and acknowledged before Qt renders it (wait %lld ms)\n",
                     static_cast<long long>(renderWaitMs.load()));
        return 1;
    }
    std::printf("GPU Canvas acknowledgment and redraw checks passed (wait %lld ms)\n",
                static_cast<long long>(renderWaitMs.load()));
    return 0;
}

#include "preview_gpu_state_test.moc"
