#include "../preview_gpu_canvas.h"
#include "../preview_gpu_metrics.h"
#include "../preview_gpu_pixels.h"
#include "../preview_gpu_snapshot.h"
#include "../preview_frame.h"
#include "../../core/src/mg_core.h"
#include "../../core/tests/mg_gfx_standin.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QRegion>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
std::atomic<int> metricReports{0};
std::atomic<bool> metricReportOnWorker{false};
QThread *metricCaller = nullptr;
QMutex metricReportMutex;
QString lastMetricReport;

void captureMetric(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(QLatin1String("GPU Canvas performance: "))) {
        metricReportOnWorker.store(QThread::currentThread() != metricCaller);
        QMutexLocker lock(&metricReportMutex);
        lastMetricReport = message;
        metricReports.fetch_add(1);
    }
}

struct PresentedTexture {
    uint32_t texture = 0;
    int width = 0;
    int height = 0;
    int presents = 0;
    int releases = 0;
};

struct PresentedPixels {
    QByteArray pixels;
    int calls = 0;
    bool accept = true;
};

bool MEGLER_CB presentPixels(void *user, const void *pixels, int width, int height, int stride)
{
    PresentedPixels *result = static_cast<PresentedPixels *>(user);
    ++result->calls;
    if (!result->accept)
        return false;
    result->pixels = QByteArray(static_cast<const char *>(pixels), stride * height);
    return width > 0;
}

bool MEGLER_CB present(void *user, uint32_t texture, int width, int height)
{
    PresentedTexture *result = static_cast<PresentedTexture *>(user);
    result->texture = texture;
    result->width = width;
    result->height = height;
    ++result->presents;
    return texture != 0;
}

bool MEGLER_CB release(void *user, uint32_t)
{
    ++static_cast<PresentedTexture *>(user)->releases;
    return true;
}

bool expect(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool isBgra(const QByteArray &pixels, int width, int x, int y, uchar b, uchar g, uchar r, uchar a)
{
    const uchar *pixel = reinterpret_cast<const uchar *>(pixels.constData()) + (y * width + x) * 4;
    return pixel[0] == b && pixel[1] == g && pixel[2] == r && pixel[3] == a;
}

bool pixelsMatch(const QByteArray &left, const QByteArray &right, int tolerance)
{
    if (left.size() != right.size())
        return false;
    for (int i = 0; i < left.size(); ++i) {
        const int difference = qAbs(static_cast<int>(static_cast<uchar>(left.at(i))) -
                                    static_cast<int>(static_cast<uchar>(right.at(i))));
        if (difference > tolerance)
            return false;
    }
    return true;
}

int pixelsMaxDifference(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return 256;
    int maximum = 0;
    for (int i = 0; i < left.size(); ++i)
        maximum = qMax(maximum, qAbs(static_cast<int>(static_cast<uchar>(left.at(i))) -
                                     static_cast<int>(static_cast<uchar>(right.at(i)))));
    return maximum;
}

bool readbackPixelLayout()
{
    for (const int width : {1, 4, 7, 17}) {
        const int height = 3;
        const int stride = width * 4 + 5;
        QByteArray rgba(width * height * 4 + 1, '\0');
        for (int i = 1; i < rgba.size(); ++i)
            rgba[i] = char((i * 13 + 19) & 255);
        QByteArray bgra(stride * height + 2, '\x5a');
        copyReadbackRgbaToBgra(reinterpret_cast<const uint8_t *>(rgba.constData() + 1),
                              reinterpret_cast<uint8_t *>(bgra.data() + 1), width, height, stride);
        QByteArray expected = bgra;
        expected.fill('\x5a');
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int src = 1 + ((height - 1 - y) * width + x) * 4;
                const int dst = 1 + y * stride + x * 4;
                expected[dst] = rgba.at(src + 2);
                expected[dst + 1] = rgba.at(src + 1);
                expected[dst + 2] = rgba.at(src);
                expected[dst + 3] = rgba.at(src + 3);
            }
        }
        if (bgra != expected) return false;
    }
    return true;
}

bool texturePixel(QOpenGLFunctions *gl, GLuint texture, uchar r, uchar g, uchar b, uchar a)
{
    GLuint framebuffer = 0;
    gl->glGenFramebuffers(1, &framebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    uchar pixel[4] = {};
    if (complete)
        gl->glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const GLenum error = gl->glGetError();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &framebuffer);
    return complete && error == GL_NO_ERROR &&
           pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == a;
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (!expect(readbackPixelLayout(), "readback flips rows and preserves BGRA, padding and unaligned buffers"))
        return 1;
    if (!mg_core_init())
        return 2;

    std::atomic<bool> metricsEnabled{false};
    auto metrics = std::make_shared<PreviewGpuMetrics>([&]() { return metricsEnabled.load(); });
    const PreviewGpuMetrics::Sample disabledSample = metrics->beginPresent();
    metrics->finishPresent(false, disabledSample, 12, 34);
    metrics->recordReadbackCacheHit(PreviewGpuReadbackReason::Filter);
    metrics->recordCanvasCopy();
    const QString emptyCache = QStringLiteral(
        "readback_cache_flush=0 readback_cache_filter=0 readback_cache_self=0 readback_cache_present=0 canvas_copies_gpu=0");
    const QString disabledInitial = metrics->takeReport();
    if (!expect(!disabledSample.enabled && disabledInitial.contains(QStringLiteral("frames=0 failed=0 samples=0")) &&
                disabledInitial.endsWith(emptyCache),
                "GPU metrics are disabled by default"))
        return 1;
    metricsEnabled.store(true);
    const PreviewGpuMetrics::Sample presentedReadback = metrics->beginReadback(PreviewGpuReadbackReason::Present);
    metrics->finishReadback(presentedReadback, 11, 5, 4);
    if (!expect(metrics->takeReport().contains(QStringLiteral("readback_flush=0 readback_filter=0 readback_self=0 "
                                                              "readback_gl_us=5 readback_convert_us=4 readback_present=1")),
                "GPU metrics distinguish embedded frame readback"))
        return 1;
    const auto pixelFrame = metrics->beginPresent();
    metrics->finishPresent(true, pixelFrame, 11, 0, true, 23);
    if (!expect(metrics->takeReport().contains(QStringLiteral("pixel_submit_samples=1 pixel_submit_us=23 pixel_submit_max_us=23")) &&
                metrics->takeReport().contains(QStringLiteral("pixel_submit_samples=0 pixel_submit_us=0 pixel_submit_max_us=0")),
                "GPU metrics report and reset the separate embedded pixel submission timing"))
        return 1;
    for (const auto reason : {PreviewGpuReadbackReason::Flush, PreviewGpuReadbackReason::Filter,
                              PreviewGpuReadbackReason::Self, PreviewGpuReadbackReason::Present})
        metrics->recordReadbackCacheHit(reason);
    metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedMode);
    metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::UnsupportedBlend);
    metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::SelfBitmap);
    metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::GpuFailure);
    metrics->recordFilterFallback(PreviewGpuFilterFallbackReason::InvalidBitmap);
    metrics->recordFilterDrawError();
    for (int i = 0; i < 16; ++i) {
        const PreviewGpuMetrics::Sample sample = metrics->beginPresent();
        metrics->finishPresent(i != 3, sample, sample.sampled ? 12 : 0, sample.sampled ? 34 : 0);
        const PreviewGpuReadbackReason reason = i % 3 == 0 ? PreviewGpuReadbackReason::Flush
            : i % 3 == 1 ? PreviewGpuReadbackReason::Filter : PreviewGpuReadbackReason::Self;
        const PreviewGpuMetrics::Sample readbackSample = metrics->beginReadback(reason);
        metrics->finishReadback(readbackSample, readbackSample.sampled ? 56 : 0,
                                readbackSample.sampled ? 20 : 0, readbackSample.sampled ? 30 : 0);
    }
    const QString report = metrics->takeReport();
    if (!expect(report.contains(QStringLiteral("frames=16 failed=1 samples=1 producer_us=12 producer_max_us=12 "
                                               "qt_wait_us=34 qt_wait_max_us=34 readbacks=16 readback_samples=1 "
                                               "readback_us=56 readback_max_us=56 readback_flush=6 readback_filter=5 "
                                               "readback_self=5 readback_gl_us=20 readback_convert_us=30 readback_present=0")) &&
    report.contains(QStringLiteral("filter_fallback_mode=1 filter_fallback_blend=1 filter_fallback_self=1 "
                                   "filter_fallback_gles=1 filter_fallback_invalid=1 filter_draw_errors=1")) &&
                report.endsWith(QStringLiteral(
                    "readback_cache_flush=1 readback_cache_filter=1 readback_cache_self=1 readback_cache_present=1 canvas_copies_gpu=0")),
                "GPU metrics aggregate sampled timings and failures") ||
        !expect(metrics->takeReport().endsWith(emptyCache), "GPU cache hits reset after a report"))
        return 1;
    const PreviewGpuMetrics::Sample stale = metrics->beginPresent();
    const PreviewGpuMetrics::Sample staleReadback = metrics->beginReadback(PreviewGpuReadbackReason::Filter);
    metrics->recordReadbackCacheHit(PreviewGpuReadbackReason::Filter);
    metricsEnabled.store(false);
    metrics->finishPresent(false, stale, 999, 999);
    metrics->finishReadback(staleReadback, 999, 999, 999);
    const QString disabledReport = metrics->takeReport();
    if (!expect(disabledReport.contains(QStringLiteral("frames=0 failed=0 samples=0")) &&
                disabledReport.contains(QStringLiteral("readback_flush=0 readback_filter=0 readback_self=0 "
                                                       "readback_gl_us=0 readback_convert_us=0 readback_present=0")) &&
                disabledReport.contains(QStringLiteral("filter_fallback_mode=0 filter_fallback_blend=0 filter_fallback_self=0 "
                                                       "filter_fallback_gles=0 filter_fallback_invalid=0 filter_draw_errors=0")) &&
                disabledReport.endsWith(emptyCache),
                "GPU metrics stop and discard pending samples when disabled"))
        return 1;
    metricsEnabled.store(true);
    const PreviewGpuMetrics::Sample fresh = metrics->beginPresent();
    metrics->finishPresent(false, stale, 999, 999);
    metrics->finishReadback(staleReadback, 999, 999, 999);
    metrics->finishPresent(true, fresh, 7, 8);
    if (!expect(metrics->takeReport().contains(QStringLiteral("frames=1 failed=0 samples=1 producer_us=7")),
                "GPU metrics reject samples from an earlier opt-in"))
        return 1;

    auto asyncMetrics = std::make_shared<PreviewGpuMetrics>([&]() { return metricsEnabled.load(); });
    const PreviewGpuMetrics::Sample initial = asyncMetrics->beginPresent();
    asyncMetrics->finishPresent(true, initial, 12, 34);
    metricCaller = QThread::currentThread();
    std::atomic<bool> workerSawDisabled{false};
    auto suppressedMetrics = std::make_shared<PreviewGpuMetrics>([&]() {
        if (QThread::currentThread() == metricCaller)
            return true;
        workerSawDisabled.store(true);
        return false;
    });
    const PreviewGpuMetrics::Sample suppressedInitial = suppressedMetrics->beginPresent();
    suppressedMetrics->finishPresent(true, suppressedInitial, 12, 34);
    const QtMessageHandler previousHandler = qInstallMessageHandler(captureMetric);
    QThread::msleep(3050);
    const PreviewGpuMetrics::Sample next = asyncMetrics->beginPresent();
    asyncMetrics->finishPresent(true, next, 0, 0);
    const bool firstReportFinished = QThreadPool::globalInstance()->waitForDone(3000);
    const bool reportedBeforeNextSample = !next.sampled && firstReportFinished && metricReports.load() == 1;
    for (int i = 0; i < 48; ++i) {
        const PreviewGpuMetrics::Sample nextSample = asyncMetrics->beginPresent();
        asyncMetrics->finishPresent(true, nextSample, 12, 34);
    }
    asyncMetrics.reset();
    const bool finished = QThreadPool::globalInstance()->waitForDone(3000);
    for (int i = 0; i < 16; ++i) {
        const PreviewGpuMetrics::Sample nextSample = suppressedMetrics->beginPresent();
        suppressedMetrics->finishPresent(true, nextSample, 12, 34);
    }
    const bool suppressionFinished = QThreadPool::globalInstance()->waitForDone(3000);
    qInstallMessageHandler(previousHandler);
    if (!expect(reportedBeforeNextSample && finished && suppressionFinished && metricReports.load() == 1 &&
                metricReportOnWorker.load() && workerSawDisabled.load(),
                "GPU metrics report asynchronously after about three seconds without waiting for a sampled frame"))
        return 1;
    metricsEnabled.store(false);

    megler_gfx_paint paint = {};
    paint.color = 0xffff0000u;
    paint.style = MEGLER_GFX_FILL;
    paint.flags = MEGLER_GFX_ANTI_ALIAS;
    paint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    paint.filter_mode = -1;
    const int benchWidth = 320, benchHeight = 480, benchStride = benchWidth * 4, iterations = 32;
    QByteArray cpuPixels(benchStride * benchHeight, char(0));
    const megler_gfx_host *cpuHost = mg_gfx_standin();
    void *cpuCanvas = cpuHost->canvas_new(cpuHost->user, cpuPixels.data(), benchWidth, benchHeight, benchStride);
    if (!expect(cpuCanvas != nullptr, "CPU baseline canvas allocation"))
        return 1;
    QElapsedTimer cpuTimer;
    cpuTimer.start();
    for (int i = 0; i < iterations; ++i)
        cpuHost->draw_round_rect(cpuHost->user, cpuCanvas, 8, 8, 312, 472, 48, 48, &paint, nullptr);
    const qint64 cpuRenderMs = cpuTimer.elapsed();
    QElapsedTimer transferTimer;
    transferTimer.start();
    for (int i = 0; i < iterations; ++i)
        previewCopyFrame(cpuPixels.constData(), benchWidth, benchHeight, benchStride);
    const qint64 cpuTransferMs = transferTimer.elapsed();
    cpuHost->canvas_free(cpuHost->user, cpuCanvas);
    std::printf("CPU baseline (%d frames, %dx%d): render=%lld ms, frame-copy=%lld ms\n",
                iterations, benchWidth, benchHeight, static_cast<long long>(cpuRenderMs),
                static_cast<long long>(cpuTransferMs));
    std::fflush(stdout);

    QOffscreenSurface shareSurface;
    QOpenGLContext shareContext;
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    shareSurface.setFormat(format);
    shareSurface.create();
    shareContext.setFormat(format);
    if (!shareSurface.isValid() || !shareContext.create() || !shareContext.isOpenGLES() ||
        !shareContext.makeCurrent(&shareSurface)) {
        std::fprintf(stderr, "SKIP: offscreen OpenGL setup failed (surface=%d context=%d gles=%d current=%d version=%d.%d)\n",
                     shareSurface.isValid(), shareContext.isValid(), shareContext.isOpenGLES(),
                     QOpenGLContext::currentContext() == &shareContext, shareContext.format().majorVersion(),
                     shareContext.format().minorVersion());
        return 77;
    }

    PresentedTexture presented;
    PreviewGpuCanvasHost gpuCanvas;
    if (!expect(gpuCanvas.setMetricsEnabled([&]() { return metricsEnabled.load(); }), "configure GPU diagnostics"))
        return 1;
    if (!gpuCanvas.prepare(&shareContext, present, release, &presented)) {
        std::fprintf(stderr, "SKIP: shared offscreen GPU Canvas context is unavailable\n");
        return 77;
    }
    if (!expect(!gpuCanvas.setMetricsEnabled([]() { return false; }), "GPU diagnostics cannot be reconfigured after prepare"))
        return 1;

    megler_gfx_host host = *mg_gfx_standin();
    gpuCanvas.install(&host);
    const int width = 64, height = 64, stride = width * 4;
    QByteArray pixels(stride * height, char(0));
    void *canvas = host.canvas_new(host.user, pixels.data(), width, height, stride);
    if (!expect(canvas != nullptr, "GPU Canvas allocation"))
        return 1;
    const auto failCanvas = [&]() {
        host.canvas_free(host.user, canvas);
        shareContext.doneCurrent();
        return 1;
    };

    host.draw_round_rect(host.user, canvas, 4, 4, 60, 60, 14, 14, &paint, nullptr);
    if (!expect(host.present(host.user, canvas, 0, 0, width, height), "direct GPU texture presentation"))
        return failCanvas();
    if (!expect(presented.presents == 1 && presented.texture != 0 &&
                presented.width == width && presented.height == height,
                "present callback receives the rendered GPU texture"))
        return failCanvas();
    if (!expect(pixels.at(32 * stride + 32 * 4) == 0, "GPU presentation does not copy pixels to CPU memory"))
        return failCanvas();
    host.flush(host.user, canvas);
    if (!expect(isBgra(pixels, width, 32, 32, 0, 0, 255, 255), "rounded rectangle center is rendered"))
        return failCanvas();
    if (!expect(isBgra(pixels, width, 4, 4, 0, 0, 0, 0), "rounded rectangle corner remains transparent"))
        return failCanvas();

    if (!expect(shareContext.makeCurrent(&shareSurface), "consumer context is current"))
        return failCanvas();
    QOpenGLFunctions *gl = shareContext.functions();
    gl->initializeOpenGLFunctions();
    GLuint sentinelFramebuffer = 0, sentinelTexture = 0;
    gl->glGenFramebuffers(1, &sentinelFramebuffer);
    gl->glGenTextures(1, &sentinelTexture);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, sentinelFramebuffer);
    gl->glBindTexture(GL_TEXTURE_2D, sentinelTexture);
    const GLuint snapshot = previewGpuSnapshot(gl, presented.texture, width, height);
    GLint boundFramebuffer = 0, boundTexture = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFramebuffer);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
    if (!expect(snapshot && snapshot != presented.texture, "consumer owns a separate snapshot") ||
        !expect(static_cast<GLuint>(boundFramebuffer) == sentinelFramebuffer &&
                static_cast<GLuint>(boundTexture) == sentinelTexture, "snapshot restores GL bindings") ||
        !expect(!previewGpuSnapshot(gl, 0, width, height), "snapshot rejects an invalid source"))
        return failCanvas();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glDeleteFramebuffers(1, &sentinelFramebuffer);
    gl->glDeleteTextures(1, &sentinelTexture);

    host.draw_color(host.user, canvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
    if (!expect(host.present(host.user, canvas, 0, 0, width, height), "present modified source") ||
        !expect(shareContext.makeCurrent(&shareSurface), "restore consumer context"))
        return failCanvas();
    const GLuint nextSnapshot = previewGpuSnapshot(gl, presented.texture, width, height);
    const bool snapshotsOk = expect(nextSnapshot && texturePixel(gl, snapshot, 255, 0, 0, 255),
                                    "previous snapshot remains unchanged after producer draws") &&
                             expect(texturePixel(gl, nextSnapshot, 0, 255, 0, 255),
                                    "new snapshot contains the new frame") &&
                             expect(previewGpuSnapshot(gl, presented.texture, width, height, snapshot) == snapshot &&
                                    texturePixel(gl, snapshot, 0, 255, 0, 255),
                                    "consumer reuses its snapshot for the new frame") &&
                             expect(!previewGpuSnapshot(gl, presented.texture, width, height, presented.texture),
                                    "producer texture cannot be reused as a snapshot") &&
                             expect(!previewGpuSnapshot(gl, presented.texture, width + 1, height, snapshot) &&
                                    gl->glIsTexture(snapshot),
                                    "failed reuse preserves the consumer-owned texture");
    gl->glDeleteTextures(1, &snapshot);
    if (!snapshotsOk) {
        if (nextSnapshot)
            gl->glDeleteTextures(1, &nextSnapshot);
        return failCanvas();
    }
    host.draw_color(host.user, canvas, 0, MEGLER_GFX_BLEND_SRC);
    host.draw_round_rect(host.user, canvas, 4, 4, 60, 60, 14, 14, &paint, nullptr);

    host.save(host.user, canvas);
    const float rounded[] = {
        MEGLER_GFX_PATH_MOVE, 12, 0,
        MEGLER_GFX_PATH_LINE, 52, 0,
        MEGLER_GFX_PATH_QUAD, 64, 0, 64, 12,
        MEGLER_GFX_PATH_LINE, 64, 52,
        MEGLER_GFX_PATH_QUAD, 64, 64, 52, 64,
        MEGLER_GFX_PATH_LINE, 12, 64,
        MEGLER_GFX_PATH_QUAD, 0, 64, 0, 52,
        MEGLER_GFX_PATH_LINE, 0, 12,
        MEGLER_GFX_PATH_QUAD, 0, 0, 12, 0,
        MEGLER_GFX_PATH_CLOSE
    };
    host.clip_path(host.user, canvas, rounded, sizeof(rounded) / sizeof(rounded[0]), MEGLER_GFX_WINDING,
                   MEGLER_GFX_CLIP_INTERSECT);
    host.draw_color(host.user, canvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
    host.restore(host.user, canvas);
    host.flush(host.user, canvas);
    if (!expect(isBgra(pixels, width, 32, 32, 0, 255, 0, 255), "path-clipped center is rendered"))
        return failCanvas();
    if (!expect(isBgra(pixels, width, 0, 0, 0, 0, 0, 0), "rounded path clip excludes its corner"))
        return failCanvas();

    QByteArray filterReferencePixels(stride * height, char(0));
    void *filterReference = cpuHost->canvas_new(cpuHost->user, filterReferencePixels.data(), width, height, stride);
    if (!expect(filterReference != nullptr, "filter readback reference canvas allocation"))
        return failCanvas();
    megler_gfx_paint filterPaint = paint;
    filterPaint.flags = 0;
    filterPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    filterPaint.filter_mode = MEGLER_GFX_BLEND_SRC;
    filterPaint.filter_color = 0xff00ff00u;
    host.draw_color(host.user, canvas, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
    cpuHost->draw_color(cpuHost->user, filterReference, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
    host.draw_round_rect(host.user, canvas, 8, 8, 56, 56, 12, 12, &filterPaint, nullptr);
    cpuHost->draw_round_rect(cpuHost->user, filterReference, 8, 8, 56, 56, 12, 12, &filterPaint, nullptr);
    filterPaint.filter_color = 0xff0000ffu;
    host.draw_oval(host.user, canvas, 20, 20, 44, 44, &filterPaint, nullptr);
    cpuHost->draw_oval(cpuHost->user, filterReference, 20, 20, 44, 44, &filterPaint, nullptr);
    megler_gfx_paint gpuPaint = paint;
    gpuPaint.flags = 0;
    gpuPaint.color = 0xff00ffffu;
    gpuPaint.blend = MEGLER_GFX_BLEND_SRC;
    gpuPaint.filter_mode = -1;
    host.draw_rect(host.user, canvas, 2, 2, 8, 8, &gpuPaint, nullptr);
    cpuHost->draw_rect(cpuHost->user, filterReference, 2, 2, 8, 8, &gpuPaint, nullptr);
    filterPaint.filter_color = 0xffffff00u;
    host.draw_rect(host.user, canvas, 30, 30, 40, 40, &filterPaint, nullptr);
    cpuHost->draw_rect(cpuHost->user, filterReference, 30, 30, 40, 40, &filterPaint, nullptr);
    host.flush(host.user, canvas);
    const bool filterPixelsMatch =
        isBgra(pixels, width, 1, 1, 0, 0, 255, 255) &&
        isBgra(pixels, width, 12, 32, 0, 255, 0, 255) &&
        isBgra(pixels, width, 24, 32, 255, 0, 0, 255) &&
        isBgra(pixels, width, 4, 4, 255, 255, 0, 255) &&
        isBgra(pixels, width, 35, 35, 0, 255, 255, 255) &&
        isBgra(filterReferencePixels, width, 1, 1, 0, 0, 255, 255) &&
        isBgra(filterReferencePixels, width, 12, 32, 0, 255, 0, 255) &&
        isBgra(filterReferencePixels, width, 24, 32, 255, 0, 0, 255) &&
        isBgra(filterReferencePixels, width, 4, 4, 255, 255, 0, 255) &&
        isBgra(filterReferencePixels, width, 35, 35, 0, 255, 255, 255);
    cpuHost->canvas_free(cpuHost->user, filterReference);
    if (!expect(filterPixelsMatch, "cached filter readback preserves GPU and CPU drawing results"))
        return failCanvas();

    const int filterWidth = 64, filterHeight = 64, filterStride = filterWidth * 4;
    QByteArray filteredGpuPixels(filterStride * filterHeight, char(0x5a));
    QByteArray filteredCpuPixels(filterStride * filterHeight, char(0x5a));
    void *filteredGpuCanvas = host.canvas_new(host.user, filteredGpuPixels.data(), filterWidth, filterHeight, filterStride);
    void *filteredCpuCanvas = cpuHost->canvas_new(cpuHost->user, filteredCpuPixels.data(),
                                                  filterWidth, filterHeight, filterStride);
    if (!expect(filteredGpuCanvas && filteredCpuCanvas, "filtered GPU and CPU canvases allocate")) {
        if (filteredGpuCanvas)
            host.canvas_free(host.user, filteredGpuCanvas);
        if (filteredCpuCanvas)
            cpuHost->canvas_free(cpuHost->user, filteredCpuCanvas);
        return failCanvas();
    }

    metricsEnabled.store(true);
    metricCaller = QThread::currentThread();
    const int filterReportsBefore = metricReports.load();
    const QtMessageHandler priorFilterHandler = qInstallMessageHandler(captureMetric);
    const bool metricFrameStarted = host.present(host.user, filteredGpuCanvas, 0, 0, filterWidth, filterHeight);
    int explicitFlushes = 0;
    const auto flushForComparison = [&]() {
        host.flush(host.user, filteredGpuCanvas);
        ++explicitFlushes;
    };
    if (metricFrameStarted)
        QThread::msleep(3050);

    const uint32_t bitmapPixels[] = {0xffff0000u, 0x80008000u, 0x00000000u, 0xff0000ffu};
    megler_gfx_image bitmap = {};
    bitmap.pixels = bitmapPixels;
    bitmap.width = bitmap.height = 2;
    bitmap.stride = 2 * 4;
    bitmap.id = 1;
    const float bitmapSource[] = {0, 0, 2, 2};
    const float bitmapDestination[] = {4, 4, 36, 36};
    megler_gfx_paint tintPaint = paint;
    tintPaint.color = 0x80ffffffu;
    tintPaint.flags = 0;
    tintPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    tintPaint.filter_color = 0x80e04020u;
    megler_gfx_paint shapeTint = tintPaint;
    shapeTint.color = 0x9070c040u;
    const uint32_t background = 0xff102030u;
    const float translated[] = {1, 0, 5, 0, 1, 7};
    const float identityForFilter[] = {1, 0, 0, 0, 1, 0};
    const float layerBounds[] = {2, 2, 60, 60};
    bool filteredPixelsMatch = true;
    bool filteredDrawStayedGpu = true;
    for (int filterMode = MEGLER_GFX_BLEND_CLEAR; filterMode <= MEGLER_GFX_BLEND_LIGHTEN; ++filterMode) {
        tintPaint.filter_mode = filterMode;
        shapeTint.filter_mode = filterMode;
        host.draw_color(host.user, filteredGpuCanvas, background, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, background, MEGLER_GFX_BLEND_SRC);
        host.save_layer(host.user, filteredGpuCanvas, layerBounds, 203, MEGLER_GFX_BLEND_SRC_OVER);
        cpuHost->save_layer(cpuHost->user, filteredCpuCanvas, layerBounds, 203, MEGLER_GFX_BLEND_SRC_OVER);
        host.set_matrix(host.user, filteredGpuCanvas, translated);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, translated);
        host.clip_rect(host.user, filteredGpuCanvas, 4, 4, 40, 42, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 4, 4, 40, 42, MEGLER_GFX_CLIP_INTERSECT);
        host.draw_bitmap(host.user, filteredGpuCanvas, &bitmap, bitmapSource, bitmapDestination, &tintPaint);
        cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &bitmap, bitmapSource, bitmapDestination, &tintPaint);
        host.set_matrix(host.user, filteredGpuCanvas, identityForFilter);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, identityForFilter);
        host.draw_rect(host.user, filteredGpuCanvas, 28, 30, 52, 52, &shapeTint, nullptr);
        cpuHost->draw_rect(cpuHost->user, filteredCpuCanvas, 28, 30, 52, 52, &shapeTint, nullptr);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        if (filterMode == MEGLER_GFX_BLEND_CLEAR)
            filteredDrawStayedGpu = filteredGpuPixels.count(char(0x5a)) == filteredGpuPixels.size();
        flushForComparison();
        const bool modeMatches = pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!modeMatches) {
            std::fprintf(stderr, "Filter mode %d max pixel difference %d\n", filterMode,
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
            for (int i = 0, shown = 0; i < filteredGpuPixels.size() && shown < 5; i += 4) {
                if (qAbs(static_cast<int>(static_cast<uchar>(filteredGpuPixels.at(i))) -
                         static_cast<int>(static_cast<uchar>(filteredCpuPixels.at(i)))) > 2) {
                    std::fprintf(stderr, "  pixel %d: GPU BGRA=%u,%u,%u,%u CPU BGRA=%u,%u,%u,%u\n", i / 4,
                                 static_cast<uchar>(filteredGpuPixels.at(i)),
                                 static_cast<uchar>(filteredGpuPixels.at(i + 1)),
                                 static_cast<uchar>(filteredGpuPixels.at(i + 2)),
                                 static_cast<uchar>(filteredGpuPixels.at(i + 3)),
                                 static_cast<uchar>(filteredCpuPixels.at(i)),
                                 static_cast<uchar>(filteredCpuPixels.at(i + 1)),
                                 static_cast<uchar>(filteredCpuPixels.at(i + 2)),
                                 static_cast<uchar>(filteredCpuPixels.at(i + 3)));
                    ++shown;
                }
            }
        }
        filteredPixelsMatch = modeMatches && filteredPixelsMatch;
    }

    const int gpuPaintBlends[] = {
        MEGLER_GFX_BLEND_DST, MEGLER_GFX_BLEND_DST_OVER, MEGLER_GFX_BLEND_DST_OUT,
        MEGLER_GFX_BLEND_SRC_ATOP, MEGLER_GFX_BLEND_XOR, MEGLER_GFX_BLEND_PLUS,
        MEGLER_GFX_BLEND_SCREEN
    };
    const uint32_t translucentBackground = 0x80406080u;
    for (const int blendMode : gpuPaintBlends) {
        tintPaint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
        tintPaint.blend = blendMode;
        shapeTint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
        shapeTint.blend = blendMode;
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        host.save_layer(host.user, filteredGpuCanvas, layerBounds, 203, MEGLER_GFX_BLEND_SRC_OVER);
        cpuHost->save_layer(cpuHost->user, filteredCpuCanvas, layerBounds, 203, MEGLER_GFX_BLEND_SRC_OVER);
        host.set_matrix(host.user, filteredGpuCanvas, translated);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, translated);
        host.clip_rect(host.user, filteredGpuCanvas, 4, 4, 40, 42, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 4, 4, 40, 42, MEGLER_GFX_CLIP_INTERSECT);
        host.draw_bitmap(host.user, filteredGpuCanvas, &bitmap, bitmapSource, bitmapDestination, &tintPaint);
        cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &bitmap, bitmapSource, bitmapDestination, &tintPaint);
        host.set_matrix(host.user, filteredGpuCanvas, identityForFilter);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, identityForFilter);
        host.draw_rect(host.user, filteredGpuCanvas, 28, 30, 52, 52, &shapeTint, nullptr);
        cpuHost->draw_rect(cpuHost->user, filteredCpuCanvas, 28, 30, 52, 52, &shapeTint, nullptr);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        const bool modeMatches = pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!modeMatches)
            std::fprintf(stderr, "Paint blend mode %d max pixel difference %d\n", blendMode,
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        filteredPixelsMatch = modeMatches && filteredPixelsMatch;
    }

    // Keep the source CPU mirror stale through the supported GPU copies.
    megler_gfx_image sourceImage = {};
    sourceImage.width = 16;
    sourceImage.height = 12;
    sourceImage.stride = 16 * 4;
    QByteArray copyGpuPixels(sourceImage.stride * sourceImage.height, char(0));
    QByteArray copyCpuPixels = QByteArray(sourceImage.stride * sourceImage.height, char(0));
    void *copyGpu = host.canvas_new(host.user, copyGpuPixels.data(), sourceImage.width, sourceImage.height, sourceImage.stride);
    void *copyCpu = cpuHost->canvas_new(cpuHost->user, copyCpuPixels.data(), sourceImage.width, sourceImage.height, sourceImage.stride);
    bool canvasCopiesMatch = copyGpu && copyCpu && host.draw_canvas;
    bool scaledCopyMatches = false;
    bool expandedCopiesMatch = true;
    int expectedGpuCopies = 0;
    if (canvasCopiesMatch) {
        host.draw_color(host.user, copyGpu, 0x9848b060u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, copyCpu, 0x9848b060u, MEGLER_GFX_BLEND_SRC);
        host.draw_rect(host.user, copyGpu, 1, 2, 4, 6, &paint, nullptr);
        cpuHost->draw_rect(cpuHost->user, copyCpu, 1, 2, 4, 6, &paint, nullptr);
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        const float region[] = {1, 2, 12, 10};
        const float first[] = {3, 5, 14, 13};
        const float arbitraryScale[] = {3, 5, 25, 20};
        const float scaled[] = {24, 4, 46, 20};
        const float second[] = {25, 24, 36, 32};
        const float copiedRegion[] = {8, 12, 19, 20};
        megler_gfx_paint copyPaint = tintPaint;
        copyPaint.color = 0xffffffffu;
        copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
        copyPaint.filter_mode = -1;
        copyPaint.flags = 0;
        const bool hasGpuScaling = shareContext.isOpenGLES();
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.set_matrix(host.user, filteredGpuCanvas, translated);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, translated);
        host.clip_rect(host.user, filteredGpuCanvas, 3, 5, 14, 12, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 3, 5, 14, 12, MEGLER_GFX_CLIP_INTERSECT);
        const bool copiedCanvas = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, region, first, &copyPaint);
        expectedGpuCopies += copiedCanvas;
        const bool copiedArbitrary = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, region, arbitraryScale, &copyPaint);
        sourceImage.pixels = copyCpuPixels.constData();
        cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, region, first, &copyPaint);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        host.set_matrix(host.user, filteredGpuCanvas, identityForFilter);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, identityForFilter);
        sourceImage.width = filterWidth;
        sourceImage.height = filterHeight;
        sourceImage.stride = filterStride;
        sourceImage.pixels = filteredGpuPixels.data();
        copyPaint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
        host.draw_bitmap(host.user, filteredGpuCanvas, &sourceImage, copiedRegion, second, &copyPaint);
        ++expectedGpuCopies;
        sourceImage.pixels = filteredCpuPixels.data();
        cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, copiedRegion, second, &copyPaint);
        flushForComparison();
        canvasCopiesMatch = !copiedArbitrary && copiedCanvas &&
                            copyGpuPixels.count(char(0)) == copyGpuPixels.size() &&
                            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!canvasCopiesMatch) {
            std::fprintf(stderr, "Canvas copy: arbitrary scale accepted=%d expected=%d copy accepted=%d source mirror unchanged=%d max pixel difference=%d\n",
                         int(copiedArbitrary), 0, int(copiedCanvas),
                         int(copyGpuPixels.count(char(0)) == copyGpuPixels.size()),
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
            for (int i = 0, shown = 0; i < filteredGpuPixels.size() && shown < 8; i += 4) {
                if (!pixelsMatch(filteredGpuPixels.mid(i, 4), filteredCpuPixels.mid(i, 4), 2)) {
                    std::fprintf(stderr, "  (%d,%d) GPU=%s CPU=%s\n", i / 4 % filterWidth, i / 4 / filterWidth,
                                 filteredGpuPixels.mid(i, 4).toHex().constData(), filteredCpuPixels.mid(i, 4).toHex().constData());
                    ++shown;
                }
            }
        }

        host.draw_color(host.user, copyGpu, 0xff48b060u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, copyCpu, 0xff48b060u, MEGLER_GFX_BLEND_SRC);
        host.draw_rect(host.user, copyGpu, 1, 2, 4, 6, &paint, nullptr);
        cpuHost->draw_rect(cpuHost->user, copyCpu, 1, 2, 4, 6, &paint, nullptr);
        megler_gfx_paint clearPaint = paint;
        clearPaint.flags = 0;
        clearPaint.blend = MEGLER_GFX_BLEND_CLEAR;
        host.draw_rect(host.user, copyGpu, 7, 5, 10, 9, &clearPaint, nullptr);
        cpuHost->draw_rect(cpuHost->user, copyCpu, 7, 5, 10, 9, &clearPaint, nullptr);
        host.draw_color(host.user, filteredGpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.set_matrix(host.user, filteredGpuCanvas, translated);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, translated);
        // Cut the first row and column of the 2x copy to test its sampling phase.
        host.clip_rect(host.user, filteredGpuCanvas, 25, 5, 46, 20, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 25, 5, 46, 20, MEGLER_GFX_CLIP_INTERSECT);
        copyPaint.filter_mode = -1;
        copyPaint.flags = MEGLER_GFX_FILTER_BITMAP;
        host.draw_color(host.user, filteredGpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        sourceImage.width = 16;
        sourceImage.height = 12;
        sourceImage.stride = 16 * 4;
        sourceImage.pixels = copyCpuPixels.constData();
        const bool smoothedCopy = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                   region, scaled, &copyPaint);
        expectedGpuCopies += smoothedCopy;
        if (smoothedCopy)
            cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, region,
                                 scaled, &copyPaint);
        flushForComparison();
        const bool smoothedCopyMatches = smoothedCopy == hasGpuScaling &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
            copyGpuPixels.count(char(0)) == copyGpuPixels.size();
        if (!smoothedCopyMatches)
            std::fprintf(stderr, "Canvas bilinear copy: accepted=%d expected=%d max difference=%d\n",
                         int(smoothedCopy), int(hasGpuScaling),
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        expandedCopiesMatch = smoothedCopyMatches && expandedCopiesMatch;
        copyPaint.flags = 0;
        host.draw_color(host.user, filteredGpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
        copyPaint.color = 0xffffffffu;
        copyPaint.filter_mode = -1;
        const bool copiedScaled = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, region, scaled, &copyPaint);
        expectedGpuCopies += copiedScaled;
        sourceImage.width = 16;
        sourceImage.height = 12;
        sourceImage.stride = 16 * 4;
        sourceImage.pixels = copyCpuPixels.constData();
        if (hasGpuScaling)
            cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, region, scaled, &copyPaint);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        host.set_matrix(host.user, filteredGpuCanvas, identityForFilter);
        cpuHost->set_matrix(cpuHost->user, filteredCpuCanvas, identityForFilter);
        flushForComparison();
        scaledCopyMatches = copiedScaled == hasGpuScaling &&
                            copyGpuPixels.count(char(0)) == copyGpuPixels.size() &&
                            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 0);
        std::fprintf(stderr, "2x copy: %s (GPU scaling=%d accepted=%d max pixel difference=%d)\n",
                     scaledCopyMatches ? "PASS" : "FAIL", int(hasGpuScaling), int(copiedScaled),
                     pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));

        struct CopyCase { int xScale, yScale, alpha, filter, blend, layerAlpha; };
        QVector<CopyCase> cases = {
            {1, 1, 128, -1, MEGLER_GFX_BLEND_SRC_OVER, 255},
            {3, 2, 128, -1, MEGLER_GFX_BLEND_SRC_OVER, 255},
            {1, 3, 255, MEGLER_GFX_BLEND_SRC_IN, MEGLER_GFX_BLEND_SRC_OVER, 255},
            {2, 2, 128, MEGLER_GFX_BLEND_SRC_IN, MEGLER_GFX_BLEND_DST_OUT, 255},
            {2, 3, 255, -1, MEGLER_GFX_BLEND_SCREEN, 255}
        };
        for (int mode = MEGLER_GFX_BLEND_CLEAR; mode <= MEGLER_GFX_BLEND_LIGHTEN; ++mode) {
            cases.append(CopyCase{1, 1, 128, mode, MEGLER_GFX_BLEND_SRC_OVER, 203});
            cases.append(CopyCase{2, 2, 128, mode, MEGLER_GFX_BLEND_SRC_OVER, 203});
        }
        for (const CopyCase &test : cases) {
            const float destination[] = {5, 6, float(5 + 11 * test.xScale), float(6 + 8 * test.yScale)};
            host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            host.save_layer(host.user, filteredGpuCanvas, nullptr, test.layerAlpha, MEGLER_GFX_BLEND_SRC_OVER);
            cpuHost->save_layer(cpuHost->user, filteredCpuCanvas, nullptr, test.layerAlpha, MEGLER_GFX_BLEND_SRC_OVER);
            host.clip_rect(host.user, filteredGpuCanvas, 6, 7, 37, 29, MEGLER_GFX_CLIP_INTERSECT);
            cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 6, 7, 37, 29, MEGLER_GFX_CLIP_INTERSECT);
            copyPaint.color = (uint32_t(test.alpha) << 24) | 0x00ffffffu;
            copyPaint.filter_mode = test.filter;
            copyPaint.blend = test.blend;
            const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, region, destination, &copyPaint);
            const bool expectedCopy = (test.xScale == 1 && test.yScale == 1) || hasGpuScaling;
            if (copied) {
                ++expectedGpuCopies;
                cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, region, destination, &copyPaint);
            }
            host.restore(host.user, filteredGpuCanvas);
            cpuHost->restore(cpuHost->user, filteredCpuCanvas);
            flushForComparison();
            const bool matches = copied == expectedCopy &&
                pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
                copyGpuPixels.count(char(0)) == copyGpuPixels.size();
            if (!matches)
                std::fprintf(stderr, "Canvas copy %dx%d alpha=%d filter=%d blend=%d: accepted=%d expected=%d max difference=%d\n",
                             test.xScale, test.yScale, test.alpha, test.filter, test.blend, int(copied),
                             int(expectedCopy), pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
            expandedCopiesMatch = matches && expandedCopiesMatch;
        }

        megler_gfx_paint pixelPaint = paint;
        pixelPaint.flags = 0;
        pixelPaint.filter_mode = -1;
        pixelPaint.blend = MEGLER_GFX_BLEND_SRC;
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 15; ++x) {
                const uint32_t alpha = (x + y) % 5 == 0 ? 0u : ((x + y) % 3 == 0 ? 0x90u : 0xffu);
                pixelPaint.color = (alpha << 24) | (uint32_t(x * 13) << 16) |
                                   (uint32_t(y * 21) << 8) | uint32_t((x * 17 + y * 11) & 255);
                host.draw_rect(host.user, copyGpu, x + 1, y + 2, x + 2, y + 3, &pixelPaint, nullptr);
                cpuHost->draw_rect(cpuHost->user, copyCpu, x + 1, y + 2, x + 2, y + 3, &pixelPaint, nullptr);
            }
        }
        const float reducedRegion[] = {1, 2, 16, 11};
        const float evenRegion[] = {0, 0, 12, 8};
        const float evenDestination[] = {5, 6, 11, 10};
        copyPaint.color = 0x80ffffffu;
        copyPaint.filter_mode = -1;
        copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        const bool copiedEvenReduction = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                          evenRegion, evenDestination, &copyPaint);
        flushForComparison();
        const bool evenReductionMatches = !copiedEvenReduction &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 0);
        if (!evenReductionMatches)
            std::fprintf(stderr, "Canvas even reduction: accepted=%d expected=%d max difference=%d\n",
                         int(copiedEvenReduction), 0,
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        expandedCopiesMatch = evenReductionMatches && expandedCopiesMatch;

        const float smoothSource[] = {2, 3, 7, 7};
        const float smoothDestinations[][4] = {{5, 6, 25, 22}, {25, 6, 28, 8}, {-3, -2, 9, 8}};
        const QImage sourceBitmap(reinterpret_cast<const uchar *>(copyCpuPixels.constData()), 16, 12, 16 * 4,
                                  QImage::Format_ARGB32_Premultiplied);
        const QImage croppedBitmap = sourceBitmap.copy(2, 3, 5, 4);
        bool sawInterpolation = false;
        for (const auto &destination : smoothDestinations) {
            host.draw_color(host.user, filteredGpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, 0xff183050u, MEGLER_GFX_BLEND_SRC);
            host.save(host.user, filteredGpuCanvas);
            cpuHost->save(cpuHost->user, filteredCpuCanvas);
            host.clip_rect(host.user, filteredGpuCanvas, 0, 0, 45, 38, MEGLER_GFX_CLIP_INTERSECT);
            cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 0, 0, 45, 38, MEGLER_GFX_CLIP_INTERSECT);
            host.clip_rect(host.user, filteredGpuCanvas, 10, 8, 13, 11, MEGLER_GFX_CLIP_DIFFERENCE);
            cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 10, 8, 13, 11, MEGLER_GFX_CLIP_DIFFERENCE);
            copyPaint.flags = MEGLER_GFX_FILTER_BITMAP;
            copyPaint.color = 0xffffffffu;
            copyPaint.filter_mode = -1;
            copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
            const QByteArray staleTarget(filteredGpuPixels.constData(), filteredGpuPixels.size());
            const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                 smoothSource, destination, &copyPaint);
            const bool noReadback = filteredGpuPixels == staleTarget &&
                copyGpuPixels.count(char(0)) == copyGpuPixels.size();
            if (copied) {
                ++expectedGpuCopies;
                cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage,
                                     smoothSource, destination, &copyPaint);
            }
            host.restore(host.user, filteredGpuCanvas);
            cpuHost->restore(cpuHost->user, filteredCpuCanvas);
            flushForComparison();
            QImage qtReference(filterWidth, filterHeight, QImage::Format_ARGB32_Premultiplied);
            qtReference.fill(0xff183050u);
            QImage qtNearest = qtReference.copy();
            const QRectF target(destination[0], destination[1], destination[2] - destination[0],
                                destination[3] - destination[1]);
            const QRegion clip = QRegion(0, 0, 45, 38).subtracted(QRegion(10, 8, 3, 3));
            const auto renderReference = [&](QImage &image, bool smooth) {
                QPainter painter(&image);
                painter.setClipRegion(clip);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
                if (copied)
                    painter.drawImage(target, croppedBitmap, QRectF(0, 0, 5, 4));
            };
            renderReference(qtReference, true);
            renderReference(qtNearest, false);
            const QByteArray qtPixels(reinterpret_cast<const char *>(qtReference.constBits()), filterStride * filterHeight);
            const QByteArray nearestPixels(reinterpret_cast<const char *>(qtNearest.constBits()), filterStride * filterHeight);
            if (copied && !pixelsMatch(qtPixels, nearestPixels, 2))
                sawInterpolation = true;
            const bool matches = copied == hasGpuScaling && noReadback &&
                pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
                pixelsMatch(filteredGpuPixels, qtPixels, 3);
            if (!matches)
                std::fprintf(stderr, "Canvas bilinear %gx%g: accepted=%d expected=%d CPU diff=%d Qt diff=%d\n",
                             target.width(), target.height(), int(copied), int(hasGpuScaling),
                             pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels),
                             pixelsMaxDifference(filteredGpuPixels, qtPixels));
            expandedCopiesMatch = matches && expandedCopiesMatch;
        }
        expandedCopiesMatch = (sawInterpolation || !hasGpuScaling) && expandedCopiesMatch;
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.clip_rect(host.user, filteredGpuCanvas, 9, 8, 13, 11, MEGLER_GFX_CLIP_DIFFERENCE);
        cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 9, 8, 13, 11, MEGLER_GFX_CLIP_DIFFERENCE);
        copyPaint.color = 0x80ffffffu;
        copyPaint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
        copyPaint.filter_color = 0x80e04020u;
        copyPaint.blend = MEGLER_GFX_BLEND_DST_OUT;
        const QByteArray staleTarget(filteredGpuPixels.constData(), filteredGpuPixels.size());
        const bool filteredSmoothCopy = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                          smoothSource, smoothDestinations[0], &copyPaint);
        const bool noReadback = filteredGpuPixels == staleTarget &&
            copyGpuPixels.count(char(0)) == copyGpuPixels.size();
        if (filteredSmoothCopy) {
            ++expectedGpuCopies;
            cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage,
                                 smoothSource, smoothDestinations[0], &copyPaint);
        }
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        const bool filteredSmoothMatches = filteredSmoothCopy == hasGpuScaling && noReadback &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!filteredSmoothMatches)
            std::fprintf(stderr, "Canvas bilinear filtered blend: accepted=%d expected=%d max difference=%d\n",
                         int(filteredSmoothCopy), int(hasGpuScaling),
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        expandedCopiesMatch = filteredSmoothMatches && expandedCopiesMatch;
        copyPaint.flags = 0;
        copyPaint.color = 0x80ffffffu;

        const float holeDestinations[][4] = {{5, 6, 20, 15}, {5, 6, 35, 24}};
        for (const auto &destination : holeDestinations) {
            for (const int blend : {MEGLER_GFX_BLEND_SRC, MEGLER_GFX_BLEND_SRC_OVER, MEGLER_GFX_BLEND_DST_IN}) {
                host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
                cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
                host.save(host.user, filteredGpuCanvas);
                cpuHost->save(cpuHost->user, filteredCpuCanvas);
                host.clip_rect(host.user, filteredGpuCanvas, 6, 7, 32, 23, MEGLER_GFX_CLIP_INTERSECT);
                cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 6, 7, 32, 23, MEGLER_GFX_CLIP_INTERSECT);
                host.clip_rect(host.user, filteredGpuCanvas, 9, 8, 14, 12, MEGLER_GFX_CLIP_DIFFERENCE);
                cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 9, 8, 14, 12, MEGLER_GFX_CLIP_DIFFERENCE);
                host.clip_rect(host.user, filteredGpuCanvas, 17, 10, 22, 16, MEGLER_GFX_CLIP_DIFFERENCE);
                cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 17, 10, 22, 16, MEGLER_GFX_CLIP_DIFFERENCE);
                copyPaint.blend = blend;
                copyPaint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
                copyPaint.filter_color = 0x80e04020u;
                const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                     reducedRegion, destination, &copyPaint);
                const bool expectedCopy = destination[2] == 20 || hasGpuScaling;
                if (copied) {
                    ++expectedGpuCopies;
                    cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage,
                                         reducedRegion, destination, &copyPaint);
                }
                host.restore(host.user, filteredGpuCanvas);
                cpuHost->restore(cpuHost->user, filteredCpuCanvas);
                flushForComparison();
                const bool matches = copied == expectedCopy &&
                    pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
                    copyGpuPixels.count(char(0)) == copyGpuPixels.size();
                if (!matches)
                    std::fprintf(stderr, "Canvas cutout clip blend %d scaled=%d: accepted=%d expected=%d max difference=%d\n",
                                 blend, int(destination[2] != 20), int(copied), int(expectedCopy),
                                 pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
                expandedCopiesMatch = matches && expandedCopiesMatch;
            }
        }
        const float rectPath[] = {
            MEGLER_GFX_PATH_MOVE, 7, 8, MEGLER_GFX_PATH_LINE, 28, 8,
            MEGLER_GFX_PATH_LINE, 28, 21, MEGLER_GFX_PATH_LINE, 7, 21,
            MEGLER_GFX_PATH_CLOSE
        };
        const float holePath[] = {
            MEGLER_GFX_PATH_MOVE, 11, 10, MEGLER_GFX_PATH_LINE, 11, 14,
            MEGLER_GFX_PATH_LINE, 16, 14, MEGLER_GFX_PATH_LINE, 16, 10,
            MEGLER_GFX_PATH_CLOSE
        };
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.clip_path(host.user, filteredGpuCanvas, rectPath, sizeof(rectPath) / sizeof(rectPath[0]),
                       MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_path(cpuHost->user, filteredCpuCanvas, rectPath, sizeof(rectPath) / sizeof(rectPath[0]),
                           MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        host.clip_path(host.user, filteredGpuCanvas, holePath, sizeof(holePath) / sizeof(holePath[0]),
                       MEGLER_GFX_EVEN_ODD, MEGLER_GFX_CLIP_DIFFERENCE);
        cpuHost->clip_path(cpuHost->user, filteredCpuCanvas, holePath, sizeof(holePath) / sizeof(holePath[0]),
                           MEGLER_GFX_EVEN_ODD, MEGLER_GFX_CLIP_DIFFERENCE);
        const QByteArray stalePathTarget(filteredGpuPixels.constData(), filteredGpuPixels.size());
        const bool copiedRectPaths = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                       reducedRegion, holeDestinations[1], &copyPaint);
        const bool pathStayedGpu = filteredGpuPixels == stalePathTarget &&
            copyGpuPixels.count(char(0)) == copyGpuPixels.size();
        if (copiedRectPaths) {
            ++expectedGpuCopies;
            cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage,
                                 reducedRegion, holeDestinations[1], &copyPaint);
        }
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        const bool rectPathsMatch = copiedRectPaths == hasGpuScaling && pathStayedGpu &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!rectPathsMatch)
            std::fprintf(stderr, "Canvas rectangle path clips: accepted=%d expected=%d max difference=%d\n",
                         int(copiedRectPaths), int(hasGpuScaling),
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        expandedCopiesMatch = rectPathsMatch && expandedCopiesMatch;
        const float fractionalRectPath[] = {
            MEGLER_GFX_PATH_MOVE, 7.5f, 8, MEGLER_GFX_PATH_LINE, 28, 8,
            MEGLER_GFX_PATH_LINE, 28, 21, MEGLER_GFX_PATH_LINE, 7.5f, 21,
            MEGLER_GFX_PATH_CLOSE
        };
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.clip_path(host.user, filteredGpuCanvas, fractionalRectPath,
                       sizeof(fractionalRectPath) / sizeof(fractionalRectPath[0]),
                       MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_path(cpuHost->user, filteredCpuCanvas, fractionalRectPath,
                           sizeof(fractionalRectPath) / sizeof(fractionalRectPath[0]),
                           MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        const bool rejectedFractionalPath = !host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                               reducedRegion, holeDestinations[1], &copyPaint);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        expandedCopiesMatch = rejectedFractionalPath &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 0) && expandedCopiesMatch;
        host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        host.clip_path(host.user, filteredGpuCanvas, rounded, sizeof(rounded) / sizeof(rounded[0]),
                       MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_path(cpuHost->user, filteredCpuCanvas, rounded, sizeof(rounded) / sizeof(rounded[0]),
                           MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        const float holeDestination[] = {5, 6, 20, 15};
        const bool rejectedPathClip = !host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                        reducedRegion, holeDestination, &copyPaint);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        expandedCopiesMatch = rejectedPathClip &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) && expandedCopiesMatch;
        host.save(host.user, filteredGpuCanvas);
        cpuHost->save(cpuHost->user, filteredCpuCanvas);
        for (int i = 0; i < 9; ++i) {
            host.clip_rect(host.user, filteredGpuCanvas, float(i), 0, float(i + 1), 1, MEGLER_GFX_CLIP_DIFFERENCE);
            cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, float(i), 0, float(i + 1), 1, MEGLER_GFX_CLIP_DIFFERENCE);
        }
        const bool rejectedExcessCuts = !host.draw_canvas(host.user, filteredGpuCanvas, copyGpu,
                                                          reducedRegion, holeDestination, &copyPaint);
        host.restore(host.user, filteredGpuCanvas);
        cpuHost->restore(cpuHost->user, filteredCpuCanvas);
        flushForComparison();
        expandedCopiesMatch = rejectedExcessCuts &&
            pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) && expandedCopiesMatch;
        copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
        const int dimensions[][2] = {{5, 3}, {3, 3}, {15, 3}, {30, 3}, {5, 18},
                                     {1, 1}, {8, 7}, {22, 14}};
        int reductionCase = 0;
        for (const auto &size : dimensions) {
            const float destination[] = {5, 6, float(5 + size[0]), float(6 + size[1])};
            host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            host.save(host.user, filteredGpuCanvas);
            cpuHost->save(cpuHost->user, filteredCpuCanvas);
            host.clip_rect(host.user, filteredGpuCanvas, 5, 6, 32, 22, MEGLER_GFX_CLIP_INTERSECT);
            cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 5, 6, 32, 22, MEGLER_GFX_CLIP_INTERSECT);
            copyPaint.filter_mode = (reductionCase++ % 2) ? MEGLER_GFX_BLEND_MODULATE : -1;
            copyPaint.filter_color = 0x80e04020u;
            const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, reducedRegion,
                                                 destination, &copyPaint);
            if (copied) {
                ++expectedGpuCopies;
                cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, reducedRegion,
                                     destination, &copyPaint);
            }
            host.restore(host.user, filteredGpuCanvas);
            cpuHost->restore(cpuHost->user, filteredCpuCanvas);
            flushForComparison();
            const bool matches = copied == hasGpuScaling &&
                pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
                copyGpuPixels.count(char(0)) == copyGpuPixels.size();
            if (!matches)
                std::fprintf(stderr, "Canvas reduction 15x9 to %dx%d: accepted=%d expected=%d max difference=%d\n",
                             size[0], size[1], int(copied), int(hasGpuScaling),
                             pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
            expandedCopiesMatch = matches && expandedCopiesMatch;
        }
        const float croppedDestinations[][4] = {
            {-4, -2, 4, 5}, {60, 61, 68, 68}, {-4, 50, 11, 59}
        };
        copyPaint.filter_mode = MEGLER_GFX_BLEND_SRC;
        copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
        for (const auto &destination : croppedDestinations) {
            host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
            const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, reducedRegion,
                                                 destination, &copyPaint);
            const bool scaled = destination[2] - destination[0] != 15 || destination[3] - destination[1] != 9;
            const bool intersectsCanvas = QRectF(destination[0], destination[1],
                                                 destination[2] - destination[0],
                                                 destination[3] - destination[1])
                                              .intersects(QRectF(0, 0, filterWidth, filterHeight));
            if (copied) {
                if (intersectsCanvas)
                    ++expectedGpuCopies;
                cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, reducedRegion,
                                     destination, &copyPaint);
            }
            flushForComparison();
            const bool matches = copied == (!scaled || hasGpuScaling) &&
                                 pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
            if (!matches)
                std::fprintf(stderr, "Canvas cropped copy: accepted=%d max difference=%d\n",
                             int(copied), pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
            expandedCopiesMatch = matches && expandedCopiesMatch;
        }

        const int rectangularBlends[] = {
            MEGLER_GFX_BLEND_CLEAR, MEGLER_GFX_BLEND_SRC, MEGLER_GFX_BLEND_SRC_OVER,
            MEGLER_GFX_BLEND_DST_OVER, MEGLER_GFX_BLEND_SRC_IN, MEGLER_GFX_BLEND_DST_IN,
            MEGLER_GFX_BLEND_SRC_OUT, MEGLER_GFX_BLEND_DST_OUT, MEGLER_GFX_BLEND_SRC_ATOP,
            MEGLER_GFX_BLEND_DST_ATOP, MEGLER_GFX_BLEND_XOR, MEGLER_GFX_BLEND_PLUS,
            MEGLER_GFX_BLEND_MODULATE, MEGLER_GFX_BLEND_SCREEN
        };
        const float blendDestination[] = {5, 6, 20, 15};
        for (const int blend : rectangularBlends) {
            for (int filter = MEGLER_GFX_BLEND_CLEAR; filter <= MEGLER_GFX_BLEND_LIGHTEN; ++filter) {
                copyPaint.blend = blend;
                copyPaint.filter_mode = filter;
                copyPaint.filter_color = 0x80e04020u;
                host.draw_color(host.user, filteredGpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
                cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, translucentBackground, MEGLER_GFX_BLEND_SRC);
                host.save(host.user, filteredGpuCanvas);
                cpuHost->save(cpuHost->user, filteredCpuCanvas);
                host.clip_rect(host.user, filteredGpuCanvas, 7, 8, 18, 14, MEGLER_GFX_CLIP_INTERSECT);
                cpuHost->clip_rect(cpuHost->user, filteredCpuCanvas, 7, 8, 18, 14, MEGLER_GFX_CLIP_INTERSECT);
                const bool copied = host.draw_canvas(host.user, filteredGpuCanvas, copyGpu, reducedRegion,
                                                     blendDestination, &copyPaint);
                if (copied) {
                    ++expectedGpuCopies;
                    cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &sourceImage, reducedRegion,
                                         blendDestination, &copyPaint);
                }
                host.restore(host.user, filteredGpuCanvas);
                cpuHost->restore(cpuHost->user, filteredCpuCanvas);
                flushForComparison();
                const bool matches = copied && pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2) &&
                    copyGpuPixels.count(char(0)) == copyGpuPixels.size();
                if (!matches)
                    std::fprintf(stderr, "Canvas blend %d filter %d: accepted=%d max difference=%d\n",
                                 blend, filter, int(copied),
                                 pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
                expandedCopiesMatch = matches && expandedCopiesMatch;
            }
        }

        const float selfSource[] = {1, 2, 9, 8};
        const float selfDestinations[][4] = {{3, 3, 11, 9}, {0, 0, 16, 12}, {2, 1, 14, 10}};
        for (int i = 0; i < 3; ++i) {
            if (i && !hasGpuScaling)
                continue;
            if (i == 2) {
                host.clip_rect(host.user, copyGpu, 6, 3, 9, 6, MEGLER_GFX_CLIP_DIFFERENCE);
                cpuHost->clip_rect(cpuHost->user, copyCpu, 6, 3, 9, 6, MEGLER_GFX_CLIP_DIFFERENCE);
            }
            pixelPaint.color = 0xffc02860u;
            host.draw_rect(host.user, copyGpu, 1, 2, 2, 3, &pixelPaint, nullptr);
            cpuHost->draw_rect(cpuHost->user, copyCpu, 1, 2, 2, 3, &pixelPaint, nullptr);
            megler_gfx_image selfImage = {};
            selfImage.width = 16;
            selfImage.height = 12;
            selfImage.stride = 16 * 4;
            selfImage.pixels = copyGpuPixels.data();
            megler_gfx_paint selfPaint = pixelPaint;
            selfPaint.color = 0x80ffffffu;
            selfPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
            selfPaint.flags = i == 2 ? MEGLER_GFX_FILTER_BITMAP : 0;
            selfPaint.filter_mode = i == 1 ? MEGLER_GFX_BLEND_SRC_IN : -1;
            selfPaint.filter_color = 0x80e04020u;
            const QByteArray staleSelf(copyGpuPixels.constData(), copyGpuPixels.size());
            host.draw_bitmap(host.user, copyGpu, &selfImage, selfSource, selfDestinations[i], &selfPaint);
            const bool stayedGpu = copyGpuPixels == staleSelf;
            ++expectedGpuCopies;
            selfImage.pixels = copyCpuPixels.data();
            cpuHost->draw_bitmap(cpuHost->user, copyCpu, &selfImage, selfSource, selfDestinations[i], &selfPaint);
            host.flush(host.user, copyGpu);
            ++explicitFlushes;
            const bool matches = stayedGpu && pixelsMatch(copyGpuPixels, copyCpuPixels, 2);
            if (!matches)
                std::fprintf(stderr, "Canvas overlapping self-copy %d: no readback=%d max difference=%d\n",
                             i, int(stayedGpu), pixelsMaxDifference(copyGpuPixels, copyCpuPixels));
            expandedCopiesMatch = matches && expandedCopiesMatch;
        }
    }
    if (copyGpu)
        host.canvas_free(host.user, copyGpu);
    if (copyCpu)
        cpuHost->canvas_free(cpuHost->user, copyCpu);

    const bool filterPresent = host.present(host.user, filteredGpuCanvas, 0, 0, filterWidth, filterHeight);
    const bool filterReportFinished = QThreadPool::globalInstance()->waitForDone(3000);
    metricsEnabled.store(false);
    qInstallMessageHandler(priorFilterHandler);
    QString filterReport;
    {
        QMutexLocker lock(&metricReportMutex);
        filterReport = lastMetricReport;
    }
    const auto reportCount = [&filterReport](const QString &name) {
        const QString prefix = name + QLatin1Char('=');
        for (const QString &field : filterReport.split(QLatin1Char(' '))) {
            if (field.startsWith(prefix)) {
                bool ok = false;
                const int value = field.mid(prefix.size()).toInt(&ok);
                return ok ? value : -1;
            }
        }
        return -1;
    };
    const int flushReadbacks = reportCount(QStringLiteral("readback_flush"));
    const int cachedFlushes = reportCount(QStringLiteral("readback_cache_flush"));
    if (!expect(expandedCopiesMatch, "scaled and translucent Canvas copies match CPU pixels") ||
        !expect(scaledCopyMatches, "nearest-neighbor 2x Canvas copy matches CPU pixels") ||
        !expect(metricFrameStarted && filteredDrawStayedGpu,
                "supported color-filter draws do not read the target back to CPU") ||
        !expect(filteredPixelsMatch, "all color-filter modes match CPU pixels for bitmap and shape under alpha, clip and transform") ||
        !expect(canvasCopiesMatch, "GPU canvas copies match CPU pixels") ||
        !expect(filterPresent && filterReportFinished && metricReports.load() > filterReportsBefore &&
                filterReport.contains(QStringLiteral("readback_filter=0")) &&
                filterReport.contains(QStringLiteral("readback_present=0")) &&
                filterReport.contains(QStringLiteral("readback_self=0")) &&
                flushReadbacks >= 0 && cachedFlushes >= 0 &&
                flushReadbacks + cachedFlushes == explicitFlushes &&
                filterReport.contains(QStringLiteral("canvas_copies_gpu=%1").arg(expectedGpuCopies)) &&
                filterReport.contains(QStringLiteral("filter_fallback_mode=0 filter_fallback_blend=0 "
                                                     "filter_fallback_self=0 filter_fallback_gles=0 "
                                                     "filter_fallback_invalid=0 filter_draw_errors=0")),
                "supported color-filter draws report no filter or presentation readbacks")) {
        std::fprintf(stderr, "GPU copy/filter metrics (expected copies=%d flushes=%d): %s\n",
                     expectedGpuCopies, explicitFlushes, qPrintable(filterReport));
        host.canvas_free(host.user, filteredGpuCanvas);
        cpuHost->canvas_free(cpuHost->user, filteredCpuCanvas);
        return failCanvas();
    }

    metricsEnabled.store(true);
    metricCaller = QThread::currentThread();
    const int fallbackReportsBefore = metricReports.load();
    const QtMessageHandler priorFallbackHandler = qInstallMessageHandler(captureMetric);
    const bool fallbackFrameStarted = host.present(host.user, filteredGpuCanvas, 0, 0, filterWidth, filterHeight);
    if (fallbackFrameStarted)
        QThread::msleep(3050);
    host.draw_color(host.user, filteredGpuCanvas, background, MEGLER_GFX_BLEND_SRC);
    cpuHost->draw_color(cpuHost->user, filteredCpuCanvas, background, MEGLER_GFX_BLEND_SRC);
    megler_gfx_paint fallbackPaint = tintPaint;
    fallbackPaint.filter_mode = MEGLER_GFX_BLEND_LIGHTEN + 1;
    host.draw_rect(host.user, filteredGpuCanvas, 5, 5, 20, 20, &fallbackPaint, nullptr);
    cpuHost->draw_rect(cpuHost->user, filteredCpuCanvas, 5, 5, 20, 20, &fallbackPaint, nullptr);
    fallbackPaint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
    fallbackPaint.blend = MEGLER_GFX_BLEND_DARKEN;
    host.draw_rect(host.user, filteredGpuCanvas, 22, 5, 37, 20, &fallbackPaint, nullptr);
    cpuHost->draw_rect(cpuHost->user, filteredCpuCanvas, 22, 5, 37, 20, &fallbackPaint, nullptr);
    fallbackPaint.filter_mode = MEGLER_GFX_BLEND_SRC;
    fallbackPaint.blend = MEGLER_GFX_BLEND_SRC_ATOP;
    host.draw_rect(host.user, filteredGpuCanvas, 39, 5, 54, 20, &fallbackPaint, nullptr);
    cpuHost->draw_rect(cpuHost->user, filteredCpuCanvas, 39, 5, 54, 20, &fallbackPaint, nullptr);
    fallbackPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    megler_gfx_image selfImage = {};
    selfImage.pixels = filteredGpuPixels.data();
    selfImage.width = filterWidth;
    selfImage.height = filterHeight;
    selfImage.stride = filterStride;
    megler_gfx_image cpuSelfImage = selfImage;
    cpuSelfImage.pixels = filteredCpuPixels.data();
    const float selfSource[] = {0, 0, 62, static_cast<float>(filterHeight)};
    const float selfDestination[] = {1, 0, 32, static_cast<float>(filterHeight)};
    host.draw_bitmap(host.user, filteredGpuCanvas, &selfImage, selfSource, selfDestination, &fallbackPaint);
    cpuHost->draw_bitmap(cpuHost->user, filteredCpuCanvas, &cpuSelfImage, selfSource, selfDestination, &fallbackPaint);
    host.flush(host.user, filteredGpuCanvas);
    const bool fallbackPixelsMatch = pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
    const bool fallbackPresented = host.present(host.user, filteredGpuCanvas, 0, 0, filterWidth, filterHeight);
    const bool fallbackReportFinished = QThreadPool::globalInstance()->waitForDone(3000);
    metricsEnabled.store(false);
    qInstallMessageHandler(priorFallbackHandler);
    QString fallbackReport;
    {
        QMutexLocker lock(&metricReportMutex);
        fallbackReport = lastMetricReport;
    }
    host.canvas_free(host.user, filteredGpuCanvas);
    cpuHost->canvas_free(cpuHost->user, filteredCpuCanvas);
    if (!expect(fallbackFrameStarted && fallbackPixelsMatch,
                "unsupported mode, paint blend and self-bitmap fallback match CPU pixels") ||
        !expect(fallbackPresented && fallbackReportFinished && metricReports.load() > fallbackReportsBefore &&
                fallbackReport.contains(QStringLiteral("filter_fallback_mode=1 filter_fallback_blend=2 "
                                                     "filter_fallback_self=1")),
                "fallback diagnostics count unsupported modes, blends and self-bitmap filters"))
        return failCanvas();

    const float identity[] = {1, 0, 0, 0, 1, 0};
    host.set_matrix(host.user, canvas, identity);
    host.draw_color(host.user, canvas, 0, MEGLER_GFX_BLEND_SRC);
    paint.flags = 0;
    host.draw_rect(host.user, canvas, 2, 3, 14, 9, &paint, nullptr);
    host.flush(host.user, canvas);
    if (!expect(isBgra(pixels, width, 5, 5, 0, 0, 255, 255), "identity matrix preserves rectangle") ||
        !expect(isBgra(pixels, width, 20, 5, 0, 0, 0, 0), "identity matrix preserves rectangle bounds"))
        return failCanvas();

    host.draw_color(host.user, canvas, 0, MEGLER_GFX_BLEND_SRC);
    const float rotated[] = {0, -1, 50, 1, 0, 7};
    host.set_matrix(host.user, canvas, rotated);
    host.draw_rect(host.user, canvas, 5, 10, 15, 20, &paint, nullptr);
    host.flush(host.user, canvas);
    if (!expect(isBgra(pixels, width, 35, 17, 0, 0, 255, 255), "matrix rotates and translates rectangle") ||
        !expect(isBgra(pixels, width, 10, 15, 0, 0, 0, 0), "matrix moves the rectangle away from its original position"))
        return failCanvas();

    host.canvas_free(host.user, canvas);
    if (!expect(presented.releases == 3, "the release callback runs for both presented textures and the copy source")) {
        shareContext.doneCurrent();
        return 1;
    }
    if (!expect(shareContext.makeCurrent(&shareSurface), "consumer context after source release") ||
        !expect(texturePixel(gl, nextSnapshot, 0, 255, 0, 255), "snapshot survives source release")) {
        shareContext.doneCurrent();
        return 1;
    }
    gl->glDeleteTextures(1, &nextSnapshot);

    const uint32_t persistentBitmapId = 0x7ffffff0u;
    const int persistentReleasesBefore = presented.releases;
    QByteArray persistentPixels(stride * height, char(0));
    void *persistentCanvasA = host.canvas_new_bitmap(host.user, persistentPixels.data(), width, height,
                                                      stride, persistentBitmapId);
    if (persistentCanvasA) {
        for (int i = 0; i < persistentPixels.size(); i += 4) {
            persistentPixels[i] = char(0x20);
            persistentPixels[i + 1] = char(0x40);
            persistentPixels[i + 2] = char(0x80);
            persistentPixels[i + 3] = char(0xff);
        }
        host.bitmap_changed(host.user, persistentBitmapId);
    }
    void *persistentCanvasB = persistentCanvasA
        ? host.canvas_new_bitmap(host.user, persistentPixels.data(), width, height, stride, persistentBitmapId)
        : nullptr;
    const bool persistentPresentationA = persistentCanvasA &&
        host.present(host.user, persistentCanvasA, 0, 0, width, height);
    const uint32_t persistentTexture = presented.texture;
    const bool persistentPresentationB = persistentCanvasB &&
        host.present(host.user, persistentCanvasB, 0, 0, width, height);
    const bool persistentTextureReused = persistentPresentationA && persistentPresentationB &&
        persistentTexture != 0 && presented.texture == persistentTexture;
    const bool persistentUploadMatches = shareContext.makeCurrent(&shareSurface) &&
        texturePixel(gl, persistentTexture, 0x80, 0x40, 0x20, 0xff);
    if (persistentCanvasB)
        host.draw_color(host.user, persistentCanvasB, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
    if (persistentCanvasB)
        host.flush(host.user, persistentCanvasB);
    const bool persistentFlushAvoidsReadback = persistentCanvasB &&
        isBgra(persistentPixels, width, 32, 32, 0x20, 0x40, 0x80, 0xff);
    QByteArray redReference(stride * height, char(0));
    for (int i = 0; i < redReference.size(); i += 4) {
        redReference[i + 2] = char(0xff);
        redReference[i + 3] = char(0xff);
    }
    QByteArray textureCopyPixels(stride * height, char(0));
    QByteArray cpuCopyPixels(stride * height, char(0));
    void *textureCopyCanvas = host.canvas_new(host.user, textureCopyPixels.data(), width, height, stride);
    void *cpuCopyCanvas = cpuHost->canvas_new(cpuHost->user, cpuCopyPixels.data(), width, height, stride);
    megler_gfx_image persistentImage = {};
    persistentImage.pixels = persistentPixels.data();
    persistentImage.width = width;
    persistentImage.height = height;
    persistentImage.stride = stride;
    persistentImage.id = persistentBitmapId;
    persistentImage.version = 2;
    megler_gfx_image redReferenceImage = persistentImage;
    redReferenceImage.pixels = redReference.data();
    megler_gfx_paint bitmapPaint = paint;
    bitmapPaint.color = 0x80ffffffu;
    bitmapPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    bitmapPaint.filter_mode = -1;
    const float fullSource[] = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    const float fullDestination[] = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    const bool copyCanvasesReady = textureCopyCanvas && cpuCopyCanvas;
    if (copyCanvasesReady) {
        host.clip_rect(host.user, textureCopyCanvas, 8, 8, 56, 56, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, cpuCopyCanvas, 8, 8, 56, 56, MEGLER_GFX_CLIP_INTERSECT);
        host.draw_bitmap(host.user, textureCopyCanvas, &persistentImage, fullSource, fullDestination, &bitmapPaint);
        cpuHost->draw_bitmap(cpuHost->user, cpuCopyCanvas, &redReferenceImage, fullSource, fullDestination, &bitmapPaint);
        host.flush(host.user, textureCopyCanvas);
    }
    const bool textureCopyAvoidsReadback = persistentCanvasB &&
        isBgra(persistentPixels, width, 32, 32, 0x20, 0x40, 0x80, 0xff);
    const bool textureBitmapDrawMatches = copyCanvasesReady &&
        pixelsMatch(textureCopyPixels, cpuCopyPixels, 2) &&
        isBgra(textureCopyPixels, width, 32, 32, 0, 0, 0x80, 0x80) &&
        isBgra(textureCopyPixels, width, 4, 4, 0, 0, 0, 0);
    const bool persistentReadbackWorks = persistentCanvasB &&
        host.bitmap_readback(host.user, persistentBitmapId, persistentPixels.data(), width, height, stride) &&
        isBgra(persistentPixels, width, 32, 32, 0, 0, 0xff, 0xff);

    GLint maxTextureSize = 0;
    if (shareContext.makeCurrent(&shareSurface))
        gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    const int fallbackWidth = maxTextureSize > 0 && maxTextureSize < 1048576 ? maxTextureSize + 1 : 0;
    QByteArray fallbackPixels(fallbackWidth * 4, char(0));
    QByteArray fallbackReference(fallbackWidth * 4, char(0));
    QByteArray sourceReference(stride * height, char(0));
    void *fallbackCanvas = fallbackWidth
        ? host.canvas_new(host.user, fallbackPixels.data(), fallbackWidth, 1, fallbackWidth * 4) : nullptr;
    void *fallbackCpuCanvas = fallbackWidth
        ? cpuHost->canvas_new(cpuHost->user, fallbackReference.data(), fallbackWidth, 1, fallbackWidth * 4) : nullptr;
    void *referenceSourceCanvas = cpuHost->canvas_new(cpuHost->user, sourceReference.data(), width, height, stride);
    bool fallbackSourceStale = false;
    bool fallbackSourceMatches = false;
    if (persistentCanvasB && fallbackCanvas && fallbackCpuCanvas && referenceSourceCanvas) {
        host.draw_color(host.user, persistentCanvasB, 0xff0000ffu, MEGLER_GFX_BLEND_SRC);
        host.flush(host.user, persistentCanvasB);
        fallbackSourceStale = isBgra(persistentPixels, width, 0, 0, 0, 0, 0xff, 0xff);
        cpuHost->draw_color(cpuHost->user, referenceSourceCanvas, 0xff0000ffu, MEGLER_GFX_BLEND_SRC);
        host.draw_color(host.user, fallbackCanvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
        cpuHost->draw_color(cpuHost->user, fallbackCpuCanvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
        host.clip_rect(host.user, fallbackCanvas, 1, 0, 3, 1, MEGLER_GFX_CLIP_INTERSECT);
        cpuHost->clip_rect(cpuHost->user, fallbackCpuCanvas, 1, 0, 3, 1, MEGLER_GFX_CLIP_INTERSECT);
        const float row[] = {0, 0, 4, 1};
        megler_gfx_image referenceImage = persistentImage;
        referenceImage.pixels = sourceReference.data();
        host.draw_bitmap(host.user, fallbackCanvas, &persistentImage, row, row, &bitmapPaint);
        cpuHost->draw_bitmap(cpuHost->user, fallbackCpuCanvas, &referenceImage, row, row, &bitmapPaint);
        fallbackSourceMatches = pixelsMatch(fallbackPixels, fallbackReference, 2);
    }
    if (fallbackCanvas)
        host.canvas_free(host.user, fallbackCanvas);
    if (fallbackCpuCanvas)
        cpuHost->canvas_free(cpuHost->user, fallbackCpuCanvas);
    if (referenceSourceCanvas)
        cpuHost->canvas_free(cpuHost->user, referenceSourceCanvas);
    if (textureCopyCanvas)
        host.canvas_free(host.user, textureCopyCanvas);
    if (cpuCopyCanvas)
        cpuHost->canvas_free(cpuHost->user, cpuCopyCanvas);
    if (persistentCanvasB)
        host.canvas_free(host.user, persistentCanvasB);
    if (persistentCanvasA)
        host.canvas_free(host.user, persistentCanvasA);
    const bool persistentTextureRetained = presented.releases == persistentReleasesBefore + 1;
    host.bitmap_gone(host.user, persistentBitmapId);
    const bool persistentTextureReleased = presented.releases == persistentReleasesBefore + 2;
    if (!expect(persistentTextureReused && persistentUploadMatches,
                "multiple canvases reuse one persistent GPU bitmap and upload CPU changes") ||
        !expect(persistentFlushAvoidsReadback && persistentReadbackWorks,
                "GPU flush only orders work; explicit bitmap readback materializes CPU pixels") ||
        !expect(textureCopyAvoidsReadback && textureBitmapDrawMatches,
                "Bitmap drawing samples the persistent GPU texture without source readback, matching alpha and clipping") ||
        !expect(fallbackSourceStale && fallbackSourceMatches,
                "CPU fallback materializes a GPU bitmap source before alpha-blended clipped drawing") ||
        !expect(persistentTextureRetained && persistentTextureReleased,
                "persistent bitmap texture is released only after all canvases and the bitmap are gone")) {
        shareContext.doneCurrent();
        return 1;
    }

    {
        std::mutex bitmapMutex;
        std::condition_variable bitmapChanged;
        bool writeReady = false, workerCanvasCreated = false;
        std::thread worker([&]() {
            void *workerCanvas = host.canvas_new_bitmap(host.user, persistentPixels.data(), width, height,
                                                        stride, persistentBitmapId);
            if (workerCanvas) {
                host.draw_color(host.user, workerCanvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
                host.canvas_free(host.user, workerCanvas);
            }
            if (host.release_thread)
                host.release_thread(host.user);
            std::lock_guard<std::mutex> lock(bitmapMutex);
            workerCanvasCreated = workerCanvas != nullptr;
            writeReady = true;
            bitmapChanged.notify_one();
        });
        {
            std::unique_lock<std::mutex> lock(bitmapMutex);
            bitmapChanged.wait(lock, [&]() { return writeReady; });
        }
        const bool sharedContextReadback = workerCanvasCreated &&
            host.bitmap_readback(host.user, persistentBitmapId, persistentPixels.data(), width, height, stride) &&
            isBgra(persistentPixels, width, 32, 32, 0, 255, 0, 255);
        worker.join();
        host.bitmap_gone(host.user, persistentBitmapId);
        if (!expect(sharedContextReadback,
                    "shared-context GPU bitmap writes are visible to CPU readback"))
            return 1;
    }

    {
        const uint32_t sourceId = 0x7ffffff1u;
        const int width = 320, height = 480, stride = width * 4;
        QByteArray sourcePixels(stride * height, char(0));
        QByteArray sourceReference(stride * height, char(0));
        QByteArray targetPixels(stride * height, char(0));
        QByteArray targetReference(stride * height, char(0));
        void *source = nullptr;
        std::thread writer([&]() {
            source = host.canvas_new_bitmap(host.user, sourcePixels.data(), width, height, stride, sourceId);
            if (source)
                host.draw_color(host.user, source, 0x80402010u, MEGLER_GFX_BLEND_SRC);
            if (host.release_thread)
                host.release_thread(host.user);
        });
        writer.join();
        void *target = host.canvas_new(host.user, targetPixels.data(), width, height, stride);
        void *cpuSource = cpuHost->canvas_new(cpuHost->user, sourceReference.data(), width, height, stride);
        void *cpuTarget = cpuHost->canvas_new(cpuHost->user, targetReference.data(), width, height, stride);
        bool copied = false, noSourceReadback = false, matches = false;
        bool scaledCopied = false, scaledMatches = false;
        bool sampleStateRestored = false;
        bool benchMatches = true;
        qint64 gpuCopyUs = 0, cpuFallbackUs = 0;
        if (source && target && cpuSource && cpuTarget) {
            cpuHost->draw_color(cpuHost->user, cpuSource, 0x80402010u, MEGLER_GFX_BLEND_SRC);
            host.draw_color(host.user, target, 0xff204080u, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, cpuTarget, 0xff204080u, MEGLER_GFX_BLEND_SRC);
            host.clip_rect(host.user, target, 8, 8, width - 8, height - 8, MEGLER_GFX_CLIP_INTERSECT);
            cpuHost->clip_rect(cpuHost->user, cpuTarget, 8, 8, width - 8, height - 8, MEGLER_GFX_CLIP_INTERSECT);
            megler_gfx_paint copyPaint = paint;
            copyPaint.color = 0x80ffffffu;
            copyPaint.blend = MEGLER_GFX_BLEND_SRC_OVER;
            copyPaint.filter_mode = -1;
            const float region[] = {0, 0, float(width), float(height)};
            QOpenGLContext *currentContext = QOpenGLContext::currentContext();
            QOpenGLFunctions *gl = currentContext ? currentContext->functions() : nullptr;
            if (gl) {
                gl->glSampleCoverage(0.0f, GL_FALSE);
                gl->glEnable(GL_SAMPLE_COVERAGE);
                gl->glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            }
            copied = host.draw_canvas(host.user, target, source, region, region, &copyPaint);
            if (gl) {
                sampleStateRestored = gl->glIsEnabled(GL_SAMPLE_COVERAGE) &&
                                      gl->glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
                gl->glDisable(GL_SAMPLE_COVERAGE);
                gl->glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                gl->glSampleCoverage(1.0f, GL_FALSE);
            }
            noSourceReadback = sourcePixels.count(char(0)) == sourcePixels.size();
            if (copied) {
                megler_gfx_image sourceImage = {};
                sourceImage.pixels = sourceReference.data();
                sourceImage.width = width;
                sourceImage.height = height;
                sourceImage.stride = stride;
                cpuHost->draw_bitmap(cpuHost->user, cpuTarget, &sourceImage, region, region, &copyPaint);
            }
            host.flush(host.user, target);
            matches = pixelsMatch(targetPixels, targetReference, 2);
            host.draw_color(host.user, target, 0xff204080u, MEGLER_GFX_BLEND_SRC);
            cpuHost->draw_color(cpuHost->user, cpuTarget, 0xff204080u, MEGLER_GFX_BLEND_SRC);
            const float halfRegion[] = {0, 0, float(width / 2), float(height / 2)};
            scaledCopied = host.draw_canvas(host.user, target, source, halfRegion, region, &copyPaint);
            if (scaledCopied) {
                megler_gfx_image sourceImage = {};
                sourceImage.pixels = sourceReference.data();
                sourceImage.width = width;
                sourceImage.height = height;
                sourceImage.stride = stride;
                cpuHost->draw_bitmap(cpuHost->user, cpuTarget, &sourceImage, halfRegion, region, &copyPaint);
            }
            host.flush(host.user, target);
            scaledMatches = pixelsMatch(targetPixels, targetReference, 2);

            if (copied) {
                QByteArray fallbackPixels(stride * height, char(0));
                void *fallbackTarget = host.canvas_new(host.user, fallbackPixels.data(), width, height, stride);
                bool benchmarkOk = fallbackTarget != nullptr;
                const int repetitions = 32;
                const auto runCopies = [&](void *destination, bool gpu) {
                    QElapsedTimer timer;
                    timer.start();
                    for (int i = 0; i < repetitions && benchmarkOk; ++i) {
                        host.draw_color(host.user, source, (i & 1) ? 0x80402010u : 0x80204010u,
                                        MEGLER_GFX_BLEND_SRC);
                        if (gpu) {
                            benchmarkOk = host.draw_canvas(host.user, destination, source, region, region, &copyPaint);
                        } else {
                            benchmarkOk = host.bitmap_readback(host.user, sourceId, sourcePixels.data(),
                                                               width, height, stride);
                            megler_gfx_image sourceImage = {};
                            sourceImage.pixels = sourcePixels.data();
                            sourceImage.width = width;
                            sourceImage.height = height;
                            sourceImage.stride = stride;
                            if (benchmarkOk)
                                host.draw_bitmap(host.user, destination, &sourceImage, region, region, &copyPaint);
                        }
                    }
                    host.flush(host.user, destination);
                    return timer.nsecsElapsed() / 1000;
                };
                if (benchmarkOk) {
                    host.draw_color(host.user, target, 0xff204080u, MEGLER_GFX_BLEND_SRC);
                    host.draw_color(host.user, fallbackTarget, 0xff204080u, MEGLER_GFX_BLEND_SRC);
                    host.clip_rect(host.user, fallbackTarget, 8, 8, width - 8, height - 8, MEGLER_GFX_CLIP_INTERSECT);
                    gpuCopyUs = runCopies(target, true);
                    cpuFallbackUs = runCopies(fallbackTarget, false);
                    benchMatches = benchmarkOk && pixelsMatch(targetPixels, fallbackPixels, 2);
                }
                if (fallbackTarget)
                    host.canvas_free(host.user, fallbackTarget);
            }
        }
        if (source) host.canvas_free(host.user, source);
        if (target) host.canvas_free(host.user, target);
        if (cpuSource) cpuHost->canvas_free(cpuHost->user, cpuSource);
        if (cpuTarget) cpuHost->canvas_free(cpuHost->user, cpuTarget);
        host.bitmap_gone(host.user, sourceId);
        if (!expect(copied && noSourceReadback && matches && benchMatches && sampleStateRestored,
                    "cross-context canvas copy stays on GPU and preserves alpha, clipping and blend") ||
            !expect(scaledCopied && scaledMatches,
                    "cross-context scaled canvas copy preserves alpha, clipping and blend"))
            return 1;
        if (copied)
            std::printf("Cross-context copy benchmark (32 copies, 320x480): GPU=%lld us, forced CPU readback=%lld us\n",
                        static_cast<long long>(gpuCopyUs), static_cast<long long>(cpuFallbackUs));
    }

    QByteArray gpuPixels(benchStride * benchHeight, char(0));
    void *gpuBenchCanvas = host.canvas_new(host.user, gpuPixels.data(), benchWidth, benchHeight, benchStride);
    if (!expect(gpuBenchCanvas != nullptr, "GPU benchmark canvas allocation")) {
        shareContext.doneCurrent();
        return 1;
    }
    QElapsedTimer gpuTimer;
    gpuTimer.start();
    for (int i = 0; i < iterations; ++i)
        host.draw_round_rect(host.user, gpuBenchCanvas, 8, 8, 312, 472, 48, 48, &paint, nullptr);
    const qint64 gpuSubmitMs = gpuTimer.elapsed();
    QElapsedTimer presentTimer;
    presentTimer.start();
    const bool presentedBench = host.present(host.user, gpuBenchCanvas, 0, 0, benchWidth, benchHeight);
    const qint64 gpuPresentMs = presentTimer.elapsed();
    const bool avoidedCpuCopy = gpuPixels.at((benchHeight / 2) * benchStride + (benchWidth / 2) * 4) == 0;
    std::printf("bench (%d frames, %dx%d): CPU render=%lld ms, CPU frame-copy=%lld ms; GPU draw-submit=%lld ms, GPU present-sync=%lld ms, CPU bytes copied on GPU present=0\n",
                iterations, benchWidth, benchHeight,
                static_cast<long long>(cpuRenderMs), static_cast<long long>(cpuTransferMs),
                static_cast<long long>(gpuSubmitMs), static_cast<long long>(gpuPresentMs));
    if (!expect(presentedBench && avoidedCpuCopy, "GPU rendering/presentation avoids the CPU frame copy")) {
        host.canvas_free(host.user, gpuBenchCanvas);
        shareContext.doneCurrent();
        return 1;
    }
    host.canvas_free(host.user, gpuBenchCanvas);

    PresentedPixels embedded;
    PreviewGpuCanvasHost embeddedGpu;
    if (!expect(embeddedGpu.prepareCpuPresentation(&shareContext, presentPixels, &embedded),
                "prepare offscreen GPU Canvas for pixel IPC") ||
        !expect(!embeddedGpu.prepare(&shareContext, present, release, &presented),
                "cannot switch a prepared Canvas to texture presentation")) {
        shareContext.doneCurrent();
        return 1;
    }
    megler_gfx_host embeddedHost = *mg_gfx_standin();
    embeddedGpu.install(&embeddedHost);
    QByteArray embeddedPixels(stride * height, char(0));
    void *embeddedCanvas = embeddedHost.canvas_new(embeddedHost.user, embeddedPixels.data(), width, height, stride);
    if (!expect(embeddedCanvas != nullptr, "offscreen GPU Canvas allocation")) {
        shareContext.doneCurrent();
        return 1;
    }
    embeddedHost.draw_color(embeddedHost.user, embeddedCanvas, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
    const bool firstPresent = embeddedHost.present(embeddedHost.user, embeddedCanvas, 0, 0, width, height);
    const QByteArray firstFrame = embedded.pixels;
    embeddedHost.draw_color(embeddedHost.user, embeddedCanvas, 0xff00ff00u, MEGLER_GFX_BLEND_SRC);
    const bool secondPresent = embeddedHost.present(embeddedHost.user, embeddedCanvas, 0, 0, width, height);
    const bool framesOk = expect(firstPresent && secondPresent && embedded.calls == 2 &&
                                 isBgra(firstFrame, width, 32, 32, 0, 0, 255, 255) &&
                                 isBgra(embedded.pixels, width, 32, 32, 0, 255, 0, 255),
                                 "offscreen presentation copies stable, correctly ordered pixels");
    embeddedHost.draw_rect(embeddedHost.user, embeddedCanvas, 0, 0, width, height / 2, &paint, nullptr);
    const bool orientationOk = expect(embeddedHost.present(embeddedHost.user, embeddedCanvas, 0, 0, width, height) &&
                                     isBgra(embedded.pixels, width, 32, 8, 0, 0, 255, 255) &&
                                     isBgra(embedded.pixels, width, 32, 56, 0, 255, 0, 255),
                                     "offscreen presentation preserves vertical orientation");
    embedded.accept = false;
    const bool failedPresent = !embeddedHost.present(embeddedHost.user, embeddedCanvas, 0, 0, width, height);
    embeddedHost.flush(embeddedHost.user, embeddedCanvas);
    const bool fallbackOk = expect(failedPresent && isBgra(embeddedPixels, width, 32, 32, 0, 255, 0, 255),
                                   "offscreen presentation failure preserves CPU fallback");
    embeddedHost.canvas_free(embeddedHost.user, embeddedCanvas);
    if (!framesOk || !orientationOk || !fallbackOk) {
        shareContext.doneCurrent();
        return 1;
    }

    {
        std::mutex mutex;
        std::condition_variable changed;
        bool ready = false, finish = false, created = false, unbound = false;
        const int contextsBefore = shareContext.shareGroup()->shares().size();
        int contextsAfter = -1;
        std::thread worker([&]() {
            QByteArray pixels(stride * height, char(0));
            void *canvas = embeddedHost.canvas_new(embeddedHost.user, pixels.data(), width, height, stride);
            if (canvas) {
                embeddedHost.draw_color(embeddedHost.user, canvas, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
                embeddedHost.flush(embeddedHost.user, canvas);
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                created = canvas != nullptr;
                unbound = !QOpenGLContext::currentContext();
                ready = true;
                changed.notify_one();
                changed.wait(lock, [&]() { return finish; });
            }
            if (canvas) embeddedHost.canvas_free(embeddedHost.user, canvas);
            embeddedHost.release_thread(embeddedHost.user);
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() { return ready; });
            contextsAfter = shareContext.shareGroup()->shares().size();
            finish = true;
        }
        changed.notify_one();
        worker.join();
        if (!expect(created && unbound && contextsAfter == contextsBefore,
                    "idle worker releases its offscreen context"))
            return 1;
    }

    {
        std::unique_ptr<PreviewGpuCanvasHost> retiring(new PreviewGpuCanvasHost);
        PresentedPixels retiringFrames;
        if (!expect(retiring->prepareCpuPresentation(&shareContext, presentPixels, &retiringFrames),
                    "prepare worker surface for teardown"))
            return 1;
        megler_gfx_host retiringHost = *mg_gfx_standin();
        retiring->install(&retiringHost);
        QByteArray mainPixels(stride * height, char(0));
        void *mainCanvas = retiringHost.canvas_new(retiringHost.user, mainPixels.data(), width, height, stride);
        std::mutex mutex;
        std::condition_variable changed;
        bool ready = false, finish = false, workerOk = false;
        std::thread worker([&]() {
            QByteArray pixels(stride * height, char(0));
            void *canvas = retiringHost.canvas_new(retiringHost.user, pixels.data(), width, height, stride);
            if (canvas) {
                retiringHost.draw_color(retiringHost.user, canvas, 0xffff0000u, MEGLER_GFX_BLEND_SRC);
                retiringHost.flush(retiringHost.user, canvas);
                retiringHost.canvas_free(retiringHost.user, canvas);
            }
            std::unique_lock<std::mutex> lock(mutex);
            workerOk = canvas && !QOpenGLContext::currentContext();
            ready = true;
            changed.notify_one();
            changed.wait(lock, [&]() { return finish; });
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() { return ready; });
        }
        if (mainCanvas) retiringHost.canvas_free(retiringHost.user, mainCanvas);
        retiring.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            finish = true;
        }
        changed.notify_one();
        worker.join();
        if (!expect(mainCanvas && workerOk, "destroy the offscreen surface while its idle worker remains alive"))
            return 1;
    }

    {
        PreviewGpuCanvasHost remoteGpu;
        PresentedTexture remote;
        megler_gfx_host remoteHost = *mg_gfx_standin();
        if (!expect(remoteGpu.prepareRemotePresentation(&shareContext, present, release, &remote),
                    "prepare remote texture worker"))
            return 1;
        remoteGpu.install(&remoteHost);
        QByteArray remotePixels(stride * height, char(0));
        void *canvas = remoteHost.canvas_new(remoteHost.user, remotePixels.data(), width, height, stride);
        const bool unboundAfterCall = !QOpenGLContext::currentContext();
        const bool canRelease = remoteHost.release_thread != nullptr;
        if (canRelease) remoteHost.release_thread(remoteHost.user);
        const bool unbound = !QOpenGLContext::currentContext();
        if (canvas) remoteHost.canvas_free(remoteHost.user, canvas);
        if (!expect(canvas && unboundAfterCall && canRelease && unbound,
                    "remote worker leaves the offscreen surface unbound and releases its context"))
            return 1;
    }

    shareContext.doneCurrent();
    std::puts("GPU Canvas checks passed");
    return 0;
}
