#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_H

#include "egl_frame_transport.h"
#include "embedded_gpu_transport.h"

#include <QDateTime>
#include <QMutex>
#include <QImage>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QPointer>
#include <QQuickItem>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWaitCondition>

#include <functional>
#include <memory>

struct PreviewGpuFrameCapture
{
    quint64 sequence = 0;
    QDateTime capturedAt;
    QImage beforeTransfer;
    QImage composedAfter;
    QString failure;
};

class PreviewGpuState
{
public:
    int submit(EGLDisplay display, EGLSurface surface, int width, int height, QObject *notifier, int layer = -1);
    bool submitDmaBuf(AuroraDmaBufFrame &&frame, QObject *notifier,
                      const std::function<void(quint64, bool)> &completion);
    bool setViews(const void *pixels, int width, int height, int stride, QObject *notifier);
    void place(int layer, int x, int y, int width, int height, int flags, QObject *notifier);
    EGLDisplay qtDisplay() const;
    QOpenGLContext *qtContext() const;
    bool presentCanvas(uint32_t texture, int width, int height, QObject *notifier);
    bool releaseCanvasTexture(uint32_t texture, QObject *notifier);
    bool requestFramePairCapture(quint64 sequence);
    bool framePairCaptureReady() const;
    bool takeFramePairCapture(PreviewGpuFrameCapture *capture);
    void cancelFramePairCapture(quint64 sequence, const QString &reason);
    void clear();
    void invalidate();
    QString lastFailure() const;
    void enableStoreWorkerTeardownTrace() { traceTeardown = true; }
    ~PreviewGpuState();

private:
    friend class PreviewGpuItem;
    void clearLocked();
    mutable QMutex mutex;
    QWaitCondition finished;
    QString failureReason;
    bool traceTeardown = false;
    AuroraEglFrame frame = {};
    AuroraEglFrame opaqueFrame = {};
    struct Layer {
        QRect rect;
        int flags = 0;
        quint64 generation = 0;
    } layers[8];
    QImage views;
    quint64 viewsRevision = 0;
    quint64 viewsCompleted = 0;
    bool viewsAccepted = false;
    quint64 pendingLayerGeneration = 0;
    int pendingLayer = -1;
    bool java = false;
    bool nativeFrame = false;
    bool canvasMode = false;
    bool canvasPending = false;
    bool canvasAccepted = false;
    uint32_t canvasTexture = 0;
    uint32_t canvasNodeTexture = 0;
    QSize canvasSize;
    quint64 canvasRevision = 0;
    quint64 canvasCompleted = 0;
    EGLDisplay consumerDisplay = EGL_NO_DISPLAY;
    QPointer<QOpenGLContext> consumerContext;
    bool pending = false;
    bool busy = false;
    bool done = false;
    bool accepted = false;
    bool active = false;
    quint64 generation = 0;
    AuroraDmaBufFrame dmaBufFrame;
    std::function<void(quint64, bool)> dmaBufCompletion;
    bool dmaBufPending = false;
    bool frameCaptureRequested = false;
    bool frameCaptureActive = false;
    bool frameCaptureAwaitingRender = false;
    bool frameCaptureCompleted = false;
    quint64 frameCaptureSequence = 0;
    QDateTime frameCaptureAt;
    QImage frameCaptureBefore;
    QImage frameCaptureAfter;
    QString frameCaptureFailure;
};

class PreviewGpuItem : public QQuickItem
{
public:
    PreviewGpuItem(const std::shared_ptr<PreviewGpuState> &state, QQuickItem *parent);
    ~PreviewGpuItem() override;

protected:
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;
    void releaseResources() override;

private:
    std::shared_ptr<PreviewGpuState> m_state;
    QMetaObject::Connection m_invalidation;
    QMetaObject::Connection m_afterRendering;
};

#endif
