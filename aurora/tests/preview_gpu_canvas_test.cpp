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
#include <QMutex>
#include <QMutexLocker>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QThread>

#include <atomic>
#include <cstdio>

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
    const QString emptyCache = QStringLiteral(
        "readback_cache_flush=0 readback_cache_filter=0 readback_cache_self=0 readback_cache_present=0");
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
                    "readback_cache_flush=1 readback_cache_filter=1 readback_cache_self=1 readback_cache_present=1")),
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
    shareSurface.setFormat(format);
    shareSurface.create();
    shareContext.setFormat(shareSurface.format());
    if (!shareSurface.isValid() || !shareContext.create() || !shareContext.isOpenGLES() ||
        !shareContext.makeCurrent(&shareSurface)) {
        std::fprintf(stderr, "SKIP: this Qt platform plugin cannot create an offscreen OpenGL ES context\n");
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
        host.flush(host.user, filteredGpuCanvas);
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
        host.flush(host.user, filteredGpuCanvas);
        const bool modeMatches = pixelsMatch(filteredGpuPixels, filteredCpuPixels, 2);
        if (!modeMatches)
            std::fprintf(stderr, "Paint blend mode %d max pixel difference %d\n", blendMode,
                         pixelsMaxDifference(filteredGpuPixels, filteredCpuPixels));
        filteredPixelsMatch = modeMatches && filteredPixelsMatch;
    }

    const bool filterPresent = host.present(host.user, filteredGpuCanvas, 0, 0, filterWidth, filterHeight);
    const bool filterReportFinished = QThreadPool::globalInstance()->waitForDone(3000);
    metricsEnabled.store(false);
    qInstallMessageHandler(priorFilterHandler);
    QString filterReport;
    {
        QMutexLocker lock(&metricReportMutex);
        filterReport = lastMetricReport;
    }
    if (!expect(metricFrameStarted && filteredDrawStayedGpu,
                "supported color-filter draws do not read the target back to CPU") ||
        !expect(filteredPixelsMatch, "all color-filter modes match CPU pixels for bitmap and shape under alpha, clip and transform") ||
        !expect(filterPresent && filterReportFinished && metricReports.load() > filterReportsBefore &&
                filterReport.contains(QStringLiteral("readback_filter=0")) &&
                filterReport.contains(QStringLiteral("readback_present=0")) &&
                filterReport.contains(QStringLiteral("filter_fallback_mode=0 filter_fallback_blend=0 "
                                                     "filter_fallback_self=0 filter_fallback_gles=0 "
                                                     "filter_fallback_invalid=0 filter_draw_errors=0")),
                "supported color-filter draws report no filter or presentation readbacks")) {
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
    const float selfSource[] = {0, 0, static_cast<float>(filterWidth), static_cast<float>(filterHeight)};
    const float selfDestination[] = {0, 0, static_cast<float>(filterWidth), static_cast<float>(filterHeight)};
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
    if (!expect(presented.releases == 2, "both presented textures are released after scene-graph removal")) {
        shareContext.doneCurrent();
        return 1;
    }
    if (!expect(shareContext.makeCurrent(&shareSurface), "consumer context after source release") ||
        !expect(texturePixel(gl, nextSnapshot, 0, 255, 0, 255), "snapshot survives source release")) {
        shareContext.doneCurrent();
        return 1;
    }
    gl->glDeleteTextures(1, &nextSnapshot);

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
        PreviewGpuCanvasHost remoteGpu;
        PresentedTexture remote;
        megler_gfx_host remoteHost = *mg_gfx_standin();
        if (!expect(remoteGpu.prepareRemotePresentation(&shareContext, present, release, &remote),
                    "prepare remote texture worker"))
            return 1;
        remoteGpu.install(&remoteHost);
        QByteArray remotePixels(stride * height, char(0));
        void *canvas = remoteHost.canvas_new(remoteHost.user, remotePixels.data(), width, height, stride);
        const bool current = QOpenGLContext::currentContext() &&
                             QOpenGLContext::currentContext() != &shareContext;
        const bool canRelease = remoteHost.release_thread != nullptr;
        if (canRelease) remoteHost.release_thread(remoteHost.user);
        const bool unbound = !QOpenGLContext::currentContext();
        if (canvas) remoteHost.canvas_free(remoteHost.user, canvas);
        if (!expect(canvas && current && canRelease && unbound,
                    "remote worker shutdown releases the context on its owning thread"))
            return 1;
    }

    shareContext.doneCurrent();
    std::puts("GPU Canvas checks passed");
    return 0;
}
