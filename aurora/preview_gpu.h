#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_H

#include "egl_frame_transport.h"

#include <QMutex>
#include <QImage>
#include <QQuickItem>
#include <QRect>
#include <QString>
#include <QWaitCondition>

#include <memory>

class PreviewGpuState
{
public:
    int submit(EGLDisplay display, EGLSurface surface, int width, int height, QObject *notifier, int layer = -1);
    bool setViews(const void *pixels, int width, int height, int stride, QObject *notifier);
    void place(int layer, int x, int y, int width, int height, int flags, QObject *notifier);
    EGLDisplay qtDisplay() const;
    void clear();
    void invalidate();
    QString lastFailure() const;
    ~PreviewGpuState();

private:
    friend class PreviewGpuItem;
    void clearLocked();
    mutable QMutex mutex;
    QWaitCondition finished;
    QString failureReason;
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
    EGLDisplay consumerDisplay = EGL_NO_DISPLAY;
    bool pending = false;
    bool busy = false;
    bool done = false;
    bool accepted = false;
    bool active = false;
    quint64 generation = 0;
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
};

#endif
