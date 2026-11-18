#include "preview_gpu.h"
#include "preview_gpu_order.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <climits>

namespace {
const int LayerCount = 8;

struct GpuRoot : QSGNode {
    QSGSimpleRectNode *background = new QSGSimpleRectNode;
    QSGClipNode *clip = new QSGClipNode;
    QSGTexture *native = nullptr;
    QSGTexture *views = nullptr;
    QSGTexture *layers[LayerCount] = {};
    QSGTexture *opaque[LayerCount] = {};
    quint64 viewsRevision = 0;
    quint64 layerGeneration[LayerCount] = {};
    bool java = false;

    GpuRoot()
    {
        background->setColor(Qt::black);
        appendChildNode(background);
        clip->setIsRectangular(true);
        clip->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        clip->setFlag(QSGNode::OwnsGeometry);
        appendChildNode(clip);
    }

    ~GpuRoot() override
    {
        delete clip;
        delete background;
        delete native;
        delete views;
        for (int i = 0; i < LayerCount; ++i) {
            delete layers[i];
            delete opaque[i];
        }
    }
};

void clearChildren(QSGNode *node)
{
    while (node->firstChild()) delete node->firstChild();
}

QSGTexture *importFrame(QQuickWindow *window, const AuroraEglFrame &frame, bool opaque)
{
    const GLuint texture = aurora_egl_frame_import(&frame);
    if (!texture) return nullptr;
    QQuickWindow::CreateTextureOptions options(QQuickWindow::TextureOwnsGLTexture);
    options |= opaque ? QQuickWindow::TextureIsOpaque : QQuickWindow::TextureHasAlphaChannel;
    QSGTexture *result = window->createTextureFromId(texture, QSize(frame.width, frame.height), options);
    if (!result) glDeleteTextures(1, &texture);
    return result;
}

void addTexture(QSGNode *parent, QSGTexture *texture, const QRectF &rect, bool flip)
{
    if (!texture || rect.isEmpty()) return;
    QSGSimpleTextureNode *node = new QSGSimpleTextureNode;
    node->setTexture(texture);
    node->setRect(rect);
    node->setFiltering(QSGTexture::Nearest);
    if (flip) node->setTextureCoordinatesTransform(QSGSimpleTextureNode::MirrorVertically);
    parent->appendChildNode(node);
}
}

PreviewGpuState::~PreviewGpuState()
{
    aurora_egl_frame_release(&frame);
    aurora_egl_frame_release(&opaqueFrame);
}

QString PreviewGpuState::lastFailure() const
{
    QMutexLocker lock(&mutex);
    return failureReason;
}

EGLDisplay PreviewGpuState::qtDisplay() const
{
    QMutexLocker lock(&mutex);
    return consumerDisplay;
}

int PreviewGpuState::submit(EGLDisplay display, EGLSurface surface, int width, int height, QObject *notifier, int layer)
{
    quint64 currentGeneration, layerGeneration = 0;
    if (layer < -1 || layer >= LayerCount) {
        QMutexLocker lock(&mutex);
        failureReason = QStringLiteral("invalid GPU layer index");
        return 0;
    }
    {
        QMutexLocker lock(&mutex);
        failureReason.clear();
        QElapsedTimer timer;
        timer.start();
        while (busy && consumerDisplay == display) {
            const qint64 elapsed = timer.elapsed();
            if (elapsed >= 1000) {
                failureReason = QStringLiteral("timed out waiting for the previous GPU frame");
                return 0;
            }
            finished.wait(&mutex, static_cast<unsigned long>(1000 - elapsed));
        }
        if (consumerDisplay != display) {
            failureReason = consumerDisplay == EGL_NO_DISPLAY
                ? QStringLiteral("Qt scene graph has no current GLES EGL display")
                : QStringLiteral("Qt and Android use different EGL displays");
            return 0;
        }
        busy = true;
        currentGeneration = generation;
        if (layer >= 0) layerGeneration = layers[layer].generation;
    }
    AuroraEglFrame captured = {}, capturedOpaque = {};
    char captureFailure[160] = {};
    AuroraEglFrameResult result = aurora_egl_frame_capture(display, surface, width, height, layer < 0, &captured,
                                                           captureFailure, sizeof(captureFailure));
    if (result == AURORA_EGL_FRAME_READY && layer >= 0)
        result = aurora_egl_frame_capture(display, surface, width, height, 1, &capturedOpaque,
                                          captureFailure, sizeof(captureFailure));
    if (result != AURORA_EGL_FRAME_READY) {
        aurora_egl_frame_release(&captured);
        aurora_egl_frame_release(&capturedOpaque);
        QMutexLocker lock(&mutex);
        busy = false;
        failureReason = captureFailure[0]
            ? QString::fromLatin1(captureFailure)
            : result == AURORA_EGL_FRAME_CONTEXT_LOST
                ? QStringLiteral("system EGL context was lost while capturing a frame")
                : QStringLiteral("native EGL frame capture was rejected");
        finished.wakeAll();
        return result == AURORA_EGL_FRAME_CONTEXT_LOST ? -1 : 0;
    }
    QMutexLocker lock(&mutex);
    if (consumerDisplay != display || generation != currentGeneration ||
        (layer >= 0 && layerGeneration != layers[layer].generation)) {
        aurora_egl_frame_release(&captured);
        aurora_egl_frame_release(&capturedOpaque);
        busy = false;
        failureReason = QStringLiteral("GPU frame became stale before Qt consumed it");
        finished.wakeAll();
        return 0;
    }
    frame = captured;
    opaqueFrame = capturedOpaque;
    pendingLayer = layer;
    if (layer >= 0) java = true;
    pendingLayerGeneration = layerGeneration;
    pending = true;
    done = accepted = false;
    if (!QMetaObject::invokeMethod(notifier, "requestGpuFrame", Qt::QueuedConnection)) {
        aurora_egl_frame_release(&frame);
        aurora_egl_frame_release(&opaqueFrame);
        failureReason = QStringLiteral("could not schedule a Qt scene graph update");
        pending = busy = false;
        finished.wakeAll();
        return 0;
    }
    QElapsedTimer timer;
    timer.start();
    while (!done) {
        const qint64 elapsed = timer.elapsed();
        if (elapsed >= 1000) break;
        finished.wait(&mutex, static_cast<unsigned long>(1000 - elapsed));
    }
    if (!done) {
        aurora_egl_frame_release(&frame);
        aurora_egl_frame_release(&opaqueFrame);
        failureReason = QStringLiteral("Qt scene graph did not consume the GPU frame before timeout");
        pending = active = busy = false;
        finished.wakeAll();
        return 0;
    }
    if (!accepted && failureReason.isEmpty())
        failureReason = QStringLiteral("Qt scene graph rejected the GPU frame");
    busy = false;
    finished.wakeAll();
    return accepted ? 1 : 0;
}

bool PreviewGpuState::setViews(const void *pixels, int width, int height, int stride, QObject *notifier)
{
    if (!pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 || stride < width * 4) return false;
    QImage copy = QImage(static_cast<const uchar *>(pixels), width, height, stride, QImage::Format_ARGB32_Premultiplied).copy();
    if (copy.isNull()) return false;
    QMutexLocker lock(&mutex);
    if (consumerDisplay == EGL_NO_DISPLAY) return false;
    const quint64 currentGeneration = generation;
    views = copy;
    ++viewsRevision;
    const quint64 revision = viewsRevision;
    java = active = true;
    if (!QMetaObject::invokeMethod(notifier, "requestGpuFrame", Qt::QueuedConnection)) return false;
    QElapsedTimer timer;
    timer.start();
    while (viewsCompleted < revision && generation == currentGeneration) {
        const qint64 elapsed = timer.elapsed();
        if (elapsed >= 1000) break;
        finished.wait(&mutex, static_cast<unsigned long>(1000 - elapsed));
    }
    return generation == currentGeneration && viewsCompleted >= revision && viewsAccepted;
}

void PreviewGpuState::place(int layer, int x, int y, int width, int height, int flags, QObject *notifier)
{
    if (layer < 0 || layer >= LayerCount) return;
    QMutexLocker lock(&mutex);
    Layer &l = layers[layer];
    if (width <= 0 || height <= 0) {
        l.rect = QRect();
        ++l.generation;
    } else {
        l.rect = QRect(x, y, width, height);
    }
    l.flags = flags;
    if (!QMetaObject::invokeMethod(notifier, "requestGpuFrame", Qt::QueuedConnection))
        qWarning("GPU layer update could not be scheduled");
}

void PreviewGpuState::clear()
{
    QMutexLocker lock(&mutex);
    clearLocked();
}

void PreviewGpuState::clearLocked()
{
    ++generation;
    active = java = false;
    views = QImage();
    ++viewsRevision;
    viewsAccepted = false;
    for (int i = 0; i < LayerCount; ++i) {
        layers[i].rect = QRect();
        ++layers[i].generation;
    }
    if (pending) {
        aurora_egl_frame_release(&frame);
        aurora_egl_frame_release(&opaqueFrame);
        pending = false;
        done = true;
        accepted = false;
    }
    finished.wakeAll();
}

void PreviewGpuState::invalidate()
{
    QMutexLocker lock(&mutex);
    clearLocked();
    consumerDisplay = EGL_NO_DISPLAY;
}

PreviewGpuItem::PreviewGpuItem(const std::shared_ptr<PreviewGpuState> &state, QQuickItem *parent)
    : QQuickItem(parent), m_state(state)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
    auto watchWindow = [this](QQuickWindow *window) {
        disconnect(m_invalidation);
        m_state->invalidate();
        if (window) {
            std::shared_ptr<PreviewGpuState> state = m_state;
            m_invalidation = connect(window, &QQuickWindow::sceneGraphInvalidated, this,
                                     [state]() { state->invalidate(); }, Qt::DirectConnection);
        }
    };
    connect(this, &QQuickItem::windowChanged, this, watchWindow);
    watchWindow(window());
}

PreviewGpuItem::~PreviewGpuItem() { m_state->invalidate(); }

QSGNode *PreviewGpuItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QMutexLocker lock(&m_state->mutex);
    QOpenGLContext *context = QOpenGLContext::currentContext();
    m_state->consumerDisplay = context && context->isOpenGLES() ? eglGetCurrentDisplay() : EGL_NO_DISPLAY;
    GpuRoot *root = static_cast<GpuRoot *>(oldNode);
    if (!root) root = new GpuRoot;
    clearChildren(root->clip);
    if (m_state->pending) {
        const int layer = m_state->pendingLayer;
        const bool current = layer < 0 || m_state->pendingLayerGeneration == m_state->layers[layer].generation;
        QSGTexture *first = nullptr, *second = nullptr;
        if (current && !window()) {
            m_state->failureReason = QStringLiteral("Qt scene graph window is unavailable");
        } else if (current && m_state->consumerDisplay != m_state->frame.display) {
            m_state->failureReason = QStringLiteral("Qt EGL display changed before frame import");
        } else if (current) {
            first = importFrame(window(), m_state->frame, layer < 0);
            if (!first) {
                m_state->failureReason = QStringLiteral("Qt could not import the EGLImage or wait on its GPU fence");
            } else if (layer >= 0) {
                second = importFrame(window(), m_state->opaqueFrame, true);
                if (!second)
                    m_state->failureReason = QStringLiteral("Qt could not import the opaque SurfaceView EGLImage");
            }
        }
        bool ok = !current || (first && (layer < 0 || second));
        if (ok && current) {
            m_state->failureReason.clear();
            if (layer < 0) {
                delete root->native;
                root->native = first;
                m_state->java = false;
            } else {
                delete root->layers[layer];
                delete root->opaque[layer];
                root->layers[layer] = first;
                root->opaque[layer] = second;
                root->layerGeneration[layer] = m_state->pendingLayerGeneration;
            }
        } else {
            delete first;
            delete second;
        }
        aurora_egl_frame_release(&m_state->frame);
        aurora_egl_frame_release(&m_state->opaqueFrame);
        m_state->accepted = ok;
        m_state->active = ok;
        m_state->pending = false;
        m_state->done = true;
        m_state->finished.wakeAll();
    }
    if (!m_state->active) {
        delete root;
        return nullptr;
    }
    if (m_state->java && root->viewsRevision != m_state->viewsRevision) {
        QSGTexture *views = window() && !m_state->views.isNull() ?
            window()->createTextureFromImage(m_state->views, QQuickWindow::TextureHasAlphaChannel) : nullptr;
        delete root->views;
        root->views = views;
        root->viewsRevision = m_state->viewsRevision;
        m_state->viewsCompleted = m_state->viewsRevision;
        m_state->viewsAccepted = views != nullptr;
        m_state->finished.wakeAll();
        if (!views) {
            m_state->failureReason = QStringLiteral("Qt could not upload the Java view overlay texture");
            m_state->active = false;
            delete root;
            return nullptr;
        }
    }
    const QSize sourceSize = m_state->java ? m_state->views.size() : root->native ? root->native->textureSize() : QSize();
    root->background->setRect(sourceSize.isEmpty() ? QRectF() : boundingRect());
    const QSizeF size = QSizeF(sourceSize).scaled(boundingRect().size(), Qt::KeepAspectRatio);
    const QRectF target((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
    QSGGeometry::updateRectGeometry(root->clip->geometry(), target);
    root->clip->markDirty(QSGNode::DirtyGeometry);
    root->clip->setClipRect(target);
    if (!m_state->java) {
        addTexture(root->clip, root->native, target, true);
    } else if (sourceSize.width() > 0 && sourceSize.height() > 0) {
        PreviewGpuOrderLayer orderLayers[LayerCount];
        int order[LayerCount + 1];
        for (int i = 0; i < LayerCount; ++i) {
            orderLayers[i].shown = root->layers[i] && root->layerGeneration[i] == m_state->layers[i].generation &&
                                   !m_state->layers[i].rect.isEmpty();
            orderLayers[i].flags = m_state->layers[i].flags;
        }
        const int count = previewGpuOrder(orderLayers, LayerCount, order);
        for (int i = 0; i < count; ++i) {
            const int layer = order[i];
            if (layer < 0) {
                addTexture(root->clip, root->views, target, false);
            } else {
                const QRect &r = m_state->layers[layer].rect;
                const QRectF rect(target.left() + r.x() * target.width() / sourceSize.width(),
                                  target.top() + r.y() * target.height() / sourceSize.height(),
                                  r.width() * target.width() / sourceSize.width(),
                                  r.height() * target.height() / sourceSize.height());
                const bool translucent = (m_state->layers[layer].flags & (MEGLER_GL_ABOVE | MEGLER_GL_TRANSLUCENT)) ==
                                         (MEGLER_GL_ABOVE | MEGLER_GL_TRANSLUCENT);
                addTexture(root->clip, translucent ? root->layers[layer] : root->opaque[layer], rect, true);
            }
        }
    }
    return root;
}

void PreviewGpuItem::releaseResources() { m_state->invalidate(); }
