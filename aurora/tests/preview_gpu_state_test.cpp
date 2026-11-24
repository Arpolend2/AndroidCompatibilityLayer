#include "../preview_gpu.h"
#include "../preview_frame_export.h"
#include "../../core/include/megler.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QTemporaryDir>
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

static bool checkLayerComposition()
{
    QQuickWindow window;
    window.setGeometry(0, 0, 64, 64);
    std::shared_ptr<PreviewGpuState> holder = std::make_shared<PreviewGpuState>();
    PreviewGpuState &state = *holder;
    PreviewGpuItem *item = new PreviewGpuItem(holder, window.contentItem());
    item->setSize(QSizeF(64, 64));
    FrameNotifier notifier;
    notifier.item = item;
    window.show();
    item->update();
    QElapsedTimer startup;
    startup.start();
    while (!state.qtContext() && startup.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QOpenGLContext *share = state.qtContext();
    if (!share || !share->isOpenGLES()) return false;
    QOffscreenSurface surface;
    surface.setFormat(share->format());
    surface.create();
    if (!surface.isValid()) return false;
    std::atomic<int> requested{1}, completed{0};
    std::atomic<bool> ok{true};
    PreviewGpuFrameCapture frameCapture;
    bool frameCaptureValid = false;
    std::thread producer([&]() {
        QOpenGLContext context;
        context.setFormat(share->format());
        context.setShareContext(share);
        if (!context.create() || !context.makeCurrent(&surface)) {
            ok.store(false);
            completed.store(5);
            return;
        }
        QOpenGLFunctions *gl = context.functions();
        gl->initializeOpenGLFunctions();
        QByteArray pixels(64 * 64 * 4, char(0));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x) {
                pixels[(y * 64 + x) * 4 + 1] = char(255);
                pixels[(y * 64 + x) * 4 + 3] = char(255);
            }
        GLuint canvas = 0;
        gl->glGenTextures(1, &canvas);
        gl->glBindTexture(GL_TEXTURE_2D, canvas);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.constData());
        gl->glFinish();
        EGLDisplay display = eglGetCurrentDisplay();
        const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        const EGLint surfaceAttrs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        bool available = display != EGL_NO_DISPLAY && eglChooseConfig(display, configAttrs, &config, 1, &count) && count;
        EGLSurface eglSurface = available ? eglCreatePbufferSurface(display, config, surfaceAttrs) : EGL_NO_SURFACE;
        EGLContext guest = eglSurface != EGL_NO_SURFACE
            ? eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs) : EGL_NO_CONTEXT;
        available = guest != EGL_NO_CONTEXT && eglMakeCurrent(display, eglSurface, eglSurface, guest);
        if (available) {
            gl->glClearColor(1, 0, 0, 1);
            gl->glClear(GL_COLOR_BUFFER_BIT);
            gl->glFinish();
        }
        unsigned char pixel[4] = {};
        if (available) gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        const bool canvasReady = available && canvas && gl->glGetError() == GL_NO_ERROR &&
            pixel[0] >= 240 && pixel[1] <= 15 && pixel[2] <= 15 &&
            state.presentCanvas(canvas, 64, 64, &notifier);
        state.place(0, 0, 0, 64, 64, 0, &notifier);
        const bool captureArmed = state.requestFramePairCapture(73);
        const int submitted = state.submit(display, eglSurface, 8, 8, &notifier, 0);
        frameCaptureValid = state.takeFramePairCapture(&frameCapture) &&
            frameCapture.sequence == 73 && frameCapture.failure.isEmpty() &&
            frameCapture.beforeTransfer.size() == QSize(8, 8) &&
            frameCapture.composedAfter.size() == QSize(64, 64) &&
            frameCapture.beforeTransfer.pixelColor(4, 4).red() >= 240 &&
            frameCapture.beforeTransfer.pixelColor(4, 4).green() <= 15 &&
            frameCapture.composedAfter.pixelColor(16, 32).red() >= 240 &&
            frameCapture.composedAfter.pixelColor(48, 32).green() >= 240;
        ok.store(canvasReady && captureArmed && submitted == 1 && frameCaptureValid);
        if (!context.makeCurrent(&surface)) ok.store(false);
        if (guest != EGL_NO_CONTEXT) eglDestroyContext(display, guest);
        if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(display, eglSurface);
        completed.store(1);
        while (requested.load() < 2) QThread::msleep(1);
        QImage views(64, 64, QImage::Format_ARGB32_Premultiplied);
        views.fill(Qt::transparent);
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                views.setPixel(x, y, qRgba(0, 0, 255, 255));
        ok.store(ok.load() && state.setViews(views.constBits(), 64, 64, views.bytesPerLine(), &notifier));
        completed.store(2);
        while (requested.load() < 3) QThread::msleep(1);
        ok.store(ok.load() && state.presentCanvas(canvas, 64, 64, &notifier));
        completed.store(3);
        while (requested.load() < 4) QThread::msleep(1);
        state.place(0, 0, 0, 64, 64, MEGLER_GL_ABOVE, &notifier);
        ok.store(ok.load() && state.presentCanvas(canvas, 64, 64, &notifier));
        completed.store(4);
        while (requested.load() < 5) QThread::msleep(1);
        if (!state.releaseCanvasTexture(canvas, &notifier)) ok.store(false);
        else gl->glDeleteTextures(1, &canvas);
        completed.store(5);
        context.doneCurrent();
    });

    auto waitStep = [&](int step) {
        QElapsedTimer timer;
        timer.start();
        while (completed.load() < step && timer.elapsed() < 4000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        if (completed.load() < step || !ok.load())
            std::fprintf(stderr, "FAIL: composition step %d: %s\n", step, qPrintable(state.lastFailure()));
        return completed.load() >= step && ok.load();
    };
    auto checkPixels = [&](const QColor &left, const QColor &right) {
        const QImage frame = window.grabWindow();
        if (frame.isNull()) return false;
        const QColor a = frame.pixelColor(frame.width() / 4, frame.height() / 2);
        const QColor b = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
        auto close = [](const QColor &p, const QColor &expected) {
            return qAbs(p.red() - expected.red()) < 16 && qAbs(p.green() - expected.green()) < 16 &&
                   qAbs(p.blue() - expected.blue()) < 16;
        };
        const bool matches = close(a, left) && close(b, right);
        if (!matches)
            std::fprintf(stderr, "FAIL: composition pixels left=%s right=%s expected=%s,%s\n",
                         qPrintable(a.name()), qPrintable(b.name()), qPrintable(left.name()), qPrintable(right.name()));
        return matches;
    };
    bool passed = waitStep(1) && checkPixels(Qt::red, Qt::green);
    QTemporaryDir exportRoot;
    QString exportDirectory, exportError;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool exported = frameCaptureValid && exportRoot.isValid() &&
        PreviewFrameExport::savePair(exportRoot.path(), frameCapture.sequence, frameCapture.capturedAt,
                                     frameCapture.beforeTransfer, frameCapture.composedAfter, now,
                                     &exportDirectory, &exportError) &&
        QFileInfo(QDir(exportDirectory).filePath(QStringLiteral("gl-before-transfer.png"))).isFile() &&
        QFileInfo(QDir(exportDirectory).filePath(QStringLiteral("composed-after.png"))).isFile();
    if (!exported) {
        std::fprintf(stderr, "FAIL: GPU frame-pair capture/export: %s\n", qPrintable(exportError));
        passed = false;
    }
    requested.store(2);
    passed = waitStep(2) && checkPixels(Qt::red, Qt::blue) && passed;
    requested.store(3);
    passed = waitStep(3) && checkPixels(Qt::red, Qt::green) && passed;
    requested.store(4);
    passed = waitStep(4) && checkPixels(Qt::red, Qt::red) && passed;
    requested.store(5);
    passed = waitStep(5) && passed;
    producer.join();
    return passed;
}

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setSamples(0);
    format.setAlphaBufferSize(8);
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
    if (!checkLayerComposition()) {
        std::fprintf(stderr, "FAIL: SurfaceView must be below Canvas/views, except when placed above\n");
        return 1;
    }
    std::printf("GPU Canvas acknowledgment and redraw checks passed (wait %lld ms)\n",
                static_cast<long long>(renderWaitMs.load()));
    return 0;
}

#include "preview_gpu_state_test.moc"
