#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_H

#include <QByteArray>
#include <QAtomicInt>
#include <QColor>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QString>
#include <QStringList>

#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include <memory>
class PreviewGpuState;
class PreviewGpuItem;
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
#include "preview_gpu_canvas.h"
typedef bool (MEGLER_CB *PreviewRemoteGpuCanvasPresent)(void *user, uint32_t texture, int width, int height);
#endif

#include "../core/include/megler.h"
#include "shared_folders.h"

class PreviewKeyboard;
class PreviewSurface;
class QQuickItem;
class AuroraAudioOutput;
class AuroraPickerHost;
class InstallRequestHost;
class QTimer;
class QWheelEvent;

class PreviewPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool showStatus READ showStatus NOTIFY statusChanged)
    Q_PROPERTY(int backGestureEdgeDp READ backGestureEdgeDp CONSTANT)
    Q_PROPERTY(bool closeOnBackGesture READ closeOnBackGesture CONSTANT)

public:
    PreviewPlayer(const QString &root, const QString &data, const QString &apk = QString(), QObject *parent = nullptr);
    void enableStoreWorkerTeardownTrace();
    QString status() const { return m_status; }
    bool showStatus() const { return !m_running || m_closing || frame().isNull(); }
    int renderCap() const { return m_renderCap; }
    int backGestureEdgeDp() const { return m_backGestureEdgeDp; }
    void setBackGestureEdgeDp(int width) { m_backGestureEdgeDp = width; }
    bool closeOnBackGesture() const { return m_closeOnBackGesture; }
    int exitStatus() const { return m_exitStatus; }
    void setCloseOnBackGesture(bool enabled) { m_closeOnBackGesture = enabled; }
    void setCloseOnExit(bool enabled) { m_closeOnExit = enabled; }
    void setStandaloneSettings(const QString &locale, const QString &theme, const QString &location);
    void setDeniedPermissions(const QStringList &denied) { m_deniedPermissions = denied; }
    void setSystemTheme(bool dark, const QColor &accent);
    virtual QImage frame() const;
    void start();
    void setAudio(AuroraAudioOutput *audio);
    void setPicker(AuroraPickerHost *picker);
    void setInstallHost(InstallRequestHost *host);
    void setSystemHost(const megler_system_host &host, const QString &appsDir);
    void setKeyboard(PreviewKeyboard *keyboard);
    void setRemoteKeyboard(const megler_keyboard_host &keyboard);
    void setCpuPresentationOnly(bool enabled) { m_cpuPresentationOnly = enabled; }
    void setOffscreenGpuCanvas(bool enabled) { m_offscreenGpuCanvas = enabled; }
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    void setRemoteGpuCanvasPresentation(PreviewRemoteGpuCanvasPresent present, void *user)
    {
        m_remoteGpuCanvasPresent = present;
        m_remoteGpuCanvasUser = user;
        m_remoteGpuCanvasFailed.storeRelease(0);
    }
#endif
    void setSurface(PreviewSurface *surface);
    virtual void setViewport(qreal width, qreal height, qreal pixelRatio);
    virtual bool usesGpuSurface() const { return true; }
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    std::shared_ptr<PreviewGpuState> gpuState() const { return m_gpuState; }
#endif
    virtual bool touch(int phase, float x, float y);
    Q_INVOKABLE virtual void close();
    Q_INVOKABLE virtual void closeToManager();
    Q_INVOKABLE virtual void back();
    Q_INVOKABLE bool gestureBack();
    Q_INVOKABLE bool hasFrameAfterGestureBack() const;
    Q_INVOKABLE void endGestureFrameWait();

signals:
    void statusChanged();
    void frameChanged();
    void finished();
    void returnToManagerRequested();

private slots:
    void publishFrame();
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    void requestGpuFrame();
#endif
    void ended(int status, const QString &reason);

private:
    static void MEGLER_CB presentPixels(void *user, const void *pixels, int width, int height, int stride);
    bool submitPixels(const void *pixels, int width, int height, int stride);
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    static int MEGLER_CB presentEglWindow(void *user, void *display, void *surface, int width, int height);
    static int MEGLER_CB presentEglLayer(void *user, int layer, void *display, void *surface, int width, int height);
    static bool MEGLER_CB presentEglViews(void *user, const void *pixels, int width, int height, int stride);
    static void MEGLER_CB placeEglLayer(void *user, int layer, int x, int y, int width, int height, int flags);
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    static bool MEGLER_CB presentGpuCanvas(void *user, uint32_t texture, int width, int height);
    static bool MEGLER_CB releaseGpuCanvas(void *user, uint32_t texture);
    static bool MEGLER_CB presentOffscreenGpuCanvas(void *user, const void *pixels, int width, int height, int stride);
    static bool MEGLER_CB presentRemoteGpuCanvas(void *user, uint32_t texture, int width, int height);
    static bool MEGLER_CB releaseRemoteGpuCanvas(void *user, uint32_t texture);
#endif
    static void MEGLER_CB appExited(void *user, int status, const char *reason);
    void setStatus(const QString &status);

    QString m_data;
    int m_renderCap;
    QByteArray m_localeUtf8;
    QString m_theme;
    QString m_location;
    QStringList m_deniedPermissions;
    bool m_systemDark = false;
    QColor m_systemAccent;
    QByteArray m_rootUtf8;
    QByteArray m_apkUtf8;
    QByteArray m_splitsUtf8;
    QString m_status;
    mutable QMutex m_frameMutex;
    QImage m_frame;
    QImage m_gestureFrame;
    bool m_framePending = false;
    bool m_awaitingGestureFrame = false;
    bool m_gestureFrameChanged = false;
    bool m_running = false;
    int m_exitStatus = 0;
    bool m_closing = false;
    bool m_closeOnExit = false;
    bool m_closeOnBackGesture = false;
    bool m_cpuPresentationOnly = false;
    bool m_offscreenGpuCanvas = false;
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    PreviewRemoteGpuCanvasPresent m_remoteGpuCanvasPresent = nullptr;
    void *m_remoteGpuCanvasUser = nullptr;
    QAtomicInt m_remoteGpuCanvasFailed = 0;
#endif
    bool m_hasRemoteKeyboard = false;
    megler_keyboard_host m_remoteKeyboard = {};
    int m_backGestureEdgeDp = 32;
    int m_surfaceWidth = 480;
    int m_surfaceHeight = 800;
    int m_density = 160;
    QPointer<PreviewKeyboard> m_keyboard;
    QPointer<PreviewSurface> m_surface;
    AuroraStorageHost m_storage;
    AuroraAudioOutput *m_audio = nullptr;
    AuroraPickerHost *m_picker = nullptr;
    InstallRequestHost *m_installHost = nullptr;
    megler_system_host m_systemHost = {};
    bool m_hasSystemHost = false;
    QByteArray m_appsDirUtf8;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    std::shared_ptr<PreviewGpuState> m_gpuState;
    int m_gpuDisplayWaits = 0;
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    PreviewGpuCanvasHost m_gpuCanvas;
#endif
};

class PreviewSurface : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *player READ player WRITE setPlayer NOTIFY playerChanged)
    Q_PROPERTY(qreal edgeZoneWidth MEMBER m_edgeZoneWidth)

public:
    explicit PreviewSurface(QQuickItem *parent = nullptr);
    ~PreviewSurface() override;
    QObject *player() const { return m_player; }
    void setPlayer(QObject *player);
    void paint(QPainter *painter) override;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    void requestGpuFrame();
#endif

signals:
    void playerChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    bool sendTouch(int phase, const QPointF &point);
    QRectF imageTarget(const QImage &image) const;
    void startWheelGesture();
    void advanceWheelGesture();
    void cancelWheelGesture();
    void updateViewport();
    QPointer<PreviewPlayer> m_player;
    bool m_pressed = false;
    bool m_backPressed = false;
    qreal m_edgeZoneWidth = 32;
    QPointF m_lastPoint;
    QTimer *m_wheelTimer;
    QPointF m_wheelPoint, m_wheelStart, m_wheelEnd, m_wheelLast;
    QPointF m_wheelPending;
    int m_wheelStep = 0;
    bool m_wheelActive = false;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    PreviewGpuItem *m_gpuItem = nullptr;
#endif
};

#endif
