#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_CANVAS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_CANVAS_H

#include "../core/include/megler.h"

#include <QHash>
#include <QMutex>
#include <QOpenGLContext>
#include <QPainterPath>
#include <QPointer>
#include <QSurfaceFormat>
#include <functional>
#include <memory>

class QOffscreenSurface;
class QOpenGLPaintDevice;
class QOpenGLFunctions;
class QPainter;
class PreviewGpuMetrics;
struct PreviewGpuOffscreenContexts;
enum class PreviewGpuReadbackReason;

class PreviewGpuCanvasHost
{
public:
    typedef bool (MEGLER_CB *Present)(void *user, uint32_t texture, int width, int height);
    typedef bool (MEGLER_CB *Release)(void *user, uint32_t texture);
    typedef bool (MEGLER_CB *PresentPixels)(void *user, const void *pixels, int width, int height, int stride);

    PreviewGpuCanvasHost();
    ~PreviewGpuCanvasHost();

    void enableStoreWorkerTeardownTrace() { m_traceTeardown = true; }
    bool setMetricsEnabled(const std::function<bool()> &enabled);
    bool prepare(QOpenGLContext *shareContext, Present present, Release release, void *presentUser);
    bool prepareRemotePresentation(QOpenGLContext *shareContext, Present present, Release release, void *presentUser);
    bool prepareCpuPresentation(QOpenGLContext *shareContext, PresentPixels present, void *presentUser);
    bool prepareRemotePresentationWithCpuFallback(QOpenGLContext *shareContext, Present present,
                                                   Release release, PresentPixels fallback, void *presentUser);
    bool enableNativeEglTextures();
    void *nativeEglDisplay() const { return m_nativeEglDisplay; }
    void install(megler_gfx_host *host);
    void releaseThreadContext();
    bool drawTextPath(void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader);
    bool drawTextFallbackPath(void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader);
    bool drawTextBitmap(void *canvas, const megler_gfx_image *image, const float *src, const float *dst,
                        const megler_gfx_paint *paint);

private:
    struct Canvas;
    struct Bitmap;
    struct Shader;
    struct EglTexture;
    bool prepareTexturePresentation(QOpenGLContext *shareContext, Present present, Release release,
                                    void *presentUser, bool offscreenWorker);
    static void MEGLER_CB releaseThread(void *user);
    static void *MEGLER_CB canvasNew(void *user, void *pixels, int width, int height, int stride);
    static void *MEGLER_CB canvasNewBitmap(void *user, void *pixels, int width, int height, int stride, uint32_t id);
    static void MEGLER_CB canvasFree(void *user, void *canvas);
    static void MEGLER_CB canvasDiscard(void *user, void *canvas);
    static void MEGLER_CB flush(void *user, void *canvas);
    static void MEGLER_CB touched(void *user, void *canvas);
    static bool MEGLER_CB bitmapReadback(void *user, uint32_t id, void *pixels, int width, int height, int stride);
    static void MEGLER_CB bitmapChanged(void *user, uint32_t id);
    static void MEGLER_CB bitmapGone(void *user, uint32_t id);
    static void MEGLER_CB save(void *user, void *canvas);
    static void MEGLER_CB saveLayer(void *user, void *canvas, const float *bounds, int alpha, int blend);
    static void MEGLER_CB saveLayerBlur(void *user, void *canvas, const float *bounds, int alpha, float radiusX, float radiusY, bool clamp);
    static void MEGLER_CB restore(void *user, void *canvas);
    static void MEGLER_CB setMatrix(void *user, void *canvas, const float *matrix);
    static void MEGLER_CB clipRect(void *user, void *canvas, float left, float top, float right, float bottom, int op);
    static void MEGLER_CB clipPath(void *user, void *canvas, const float *path, int count, int fill, int op);
    static void MEGLER_CB drawColor(void *user, void *canvas, uint32_t color, int blend);
    static void MEGLER_CB drawRect(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    static void MEGLER_CB drawRoundRect(void *user, void *canvas, float left, float top, float right, float bottom, float rx, float ry,
                                        const megler_gfx_paint *paint, void *shader);
    static void MEGLER_CB drawOval(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    static void MEGLER_CB drawPath(void *user, void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader);
    static void MEGLER_CB drawBitmap(void *user, void *canvas, const megler_gfx_image *image, const float *src, const float *dst,
                                     const megler_gfx_paint *paint);
    static bool MEGLER_CB drawCanvas(void *user, void *canvas, void *source, const float *src, const float *dst,
                                    const megler_gfx_paint *paint);
    static void *MEGLER_CB shaderLinear(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors,
                                        const float *positions, int count, int tile);
    static void *MEGLER_CB shaderRadial(void *user, float cx, float cy, float radius, const uint32_t *colors,
                                        const float *positions, int count, int tile);
    static void *MEGLER_CB shaderSweep(void *user, float cx, float cy, const uint32_t *colors, const float *positions, int count);
    static void *MEGLER_CB shaderBitmap(void *user, const megler_gfx_image *image, int tile);
    static void MEGLER_CB shaderMatrix(void *user, void *shader, const float *matrix);
    static void MEGLER_CB shaderFree(void *user, void *shader);
    static bool MEGLER_CB present(void *user, void *canvas, int x, int y, int width, int height);
    static int MEGLER_CB textureEglFrame(void *user, int id, void *display, void *surface, int width, int height);
    static int MEGLER_CB eglSurfaceConfigs(void *user, void *display, int version, void **configs, int capacity);
    static void *MEGLER_CB eglSurfaceNew(void *user, void *display, void *config, int width, int height, void **token);
    static void MEGLER_CB eglSurfaceFree(void *user, void *token);
    static void MEGLER_CB drawTexture(void *user, void *canvas, int id, float left, float top,
                                      float right, float bottom, int alpha);

    Canvas *findCanvas(void *canvas) const;
    bool ensureCurrent(const char *origin);
    bool ensureBitmapCurrent(Canvas *canvas);
    bool waitForBitmap(Bitmap *bitmap, bool forceWait = false);
    bool ensureCpuMirrorCurrent(Bitmap *bitmap);
    bool prepareContext(QOpenGLContext *shareContext);
    bool ensureFramebuffer(Canvas *canvas, const char *origin);
    bool ensureFilterFramebuffer(Canvas *canvas);
    bool readback(Canvas *canvas, PreviewGpuReadbackReason reason);
    bool upload(Canvas *canvas);
    void finishGpuWrite(Canvas *canvas);
    void markCpuWrite(Bitmap *bitmap);
    void removeFramebuffer(Canvas *canvas);
    bool drawFiltered(Canvas *canvas, const megler_gfx_paint *paint,
                      const QRectF &deviceBounds, const std::function<void(QPainter &)> &drawSource,
                      Canvas *copyCanvas = nullptr, const QRect &copySource = QRect());
    bool copyCanvasPixels(Canvas *canvas, Canvas *source, const float *src, const float *dst,
                          const megler_gfx_paint *paint);
    bool drawFilteredPath(Canvas *canvas, const QPainterPath &path, const megler_gfx_paint *paint, void *shader);
    bool drawFilteredBitmap(Canvas *canvas, const megler_gfx_image *image, const float *src, const float *dst,
                            const megler_gfx_paint *paint);
    void paintPath(Canvas *canvas, const QPainterPath &path, const megler_gfx_paint *paint, void *shader);
    void clip(Canvas *canvas, const QPainterPath &path, int op, bool rectangular = false);
    void *cpuShader(void *shader) const;

    const megler_gfx_host *m_cpu;
    bool m_traceTeardown = false;
    std::shared_ptr<PreviewGpuOffscreenContexts> m_offscreenContexts;
    QPointer<QOpenGLContext> m_shareContext;
    QOffscreenSurface *m_surface;
    Present m_present;
    Release m_release;
    PresentPixels m_presentPixels;
    void *m_presentUser;
    mutable QMutex m_mutex;
    QHash<void *, Canvas *> m_canvases;
    QHash<uint32_t, Bitmap *> m_bitmaps;
    QHash<int, std::shared_ptr<EglTexture>> m_eglTextures;
    bool m_nativeEglTextures = false;
    void *m_nativeEglDisplay = nullptr;
    void *m_waylandCompositor = nullptr;
    quint64 m_nextCanvasId;
    std::shared_ptr<PreviewGpuMetrics> m_metrics;
};

bool previewGpuCanvasDrawPath(void *user, void *canvas, const float *path, int count, int fill,
                              const megler_gfx_paint *paint, void *shader);
bool previewGpuCanvasDrawFallbackPath(void *user, void *canvas, const float *path, int count, int fill,
                                      const megler_gfx_paint *paint, void *shader);
bool previewGpuCanvasDrawBitmap(void *user, void *canvas, const megler_gfx_image *image, const float *src,
                                const float *dst, const megler_gfx_paint *paint);

#endif
