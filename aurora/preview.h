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
#include <memory>
#include "shared_microg_client.h"

#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include <memory>
class PreviewGpuState;
class PreviewGpuItem;
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
#include "preview_gpu_canvas.h"
typedef bool (MEGLER_CB *PreviewRemoteGpuCanvasPresent)(void *user, uint32_t texture, int width, int height);
class EmbeddedGpuTransport;
#endif

#include "../core/include/megler.h"
#include "shared_folders.h"
#include "notification_host.h"

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
    Q_PROPERTY(QString frameCaptureStatus READ frameCaptureStatus NOTIFY frameCaptureStatusChanged)
    Q_PROPERTY(bool frameCapturePending READ frameCapturePending NOTIFY frameCaptureStatusChanged)
    Q_PROPERTY(bool frameCaptureEnabled READ frameCaptureEnabled NOTIFY frameCaptureEnabledChanged)
    Q_PROPERTY(int backGestureEdgeDp READ backGestureEdgeDp CONSTANT)
    Q_PROPERTY(bool closeOnBackGesture READ closeOnBackGesture CONSTANT)
    Q_PROPERTY(int orientationMask READ orientationMask NOTIFY orientationMaskChanged)

public:
    PreviewPlayer(const QString &root, const QString &data, const QString &apk = QString(), QObject *parent = nullptr);
    void enableStoreWorkerTeardownTrace();
    QString status() const { return m_status; }
    bool showStatus() const { return !m_running || m_closing || frame().isNull(); }
    QString frameCaptureStatus() const { return m_frameCaptureStatus; }
    bool frameCapturePending() const { return m_frameCapturePending; }
    bool frameCaptureEnabled() const { return m_frameCaptureEnabled; }
    int renderCap() const { return m_renderCap; }
    int backGestureEdgeDp() const { return m_backGestureEdgeDp; }
    void setBackGestureEdgeDp(int width) { m_backGestureEdgeDp = width; }
    bool closeOnBackGesture() const { return m_closeOnBackGesture; }
    int orientationMask() const { return m_orientationMask; }
    int exitStatus() const { return m_exitStatus; }
    void setCloseOnBackGesture(bool enabled) { m_closeOnBackGesture = enabled; }
    void setCloseOnExit(bool enabled) { m_closeOnExit = enabled; }
    void setCloseToBackground(bool enabled) { m_closeToBackground = enabled; }
    void resumeFromBackground();
    void setSharedMicrog(bool enabled) { m_sharedMicrogEnabled = enabled; }
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
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    void setRemoteGpuCanvasPresentation(PreviewRemoteGpuCanvasPresent present, void *user,
                                        EmbeddedGpuTransport *transport)
    {
        m_remoteGpuCanvasPresent = present;
        m_remoteGpuCanvasUser = user;
        m_remoteGpuTransport = transport;
        m_remoteGpuCanvasFailed.storeRelease(0);
    }
#endif
    void setSurface(PreviewSurface *surface);
    virtual void setViewport(qreal width, qreal height, qreal pixelRatio, int rotation = -1);
    virtual void setDisplayRotation(int rotation);
    void setScreenOrientations(int natural, int actual);
    int displayRotation() const { return m_displayRotation; }
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
    Q_INVOKABLE void captureFramePair();

signals:
    void statusChanged();
    void frameCaptureStatusChanged();
    void frameCaptureEnabledChanged();
    void orientationMaskChanged();
    void frameChanged();
    void finished();
    void returnToManagerRequested();

private slots:
    void publishFrame();
    void setRequestedOrientation(int requested);
    void finishFrameCapture();
    void updateFrameCaptureEnabled();
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
    void queueFrameCaptureResult();
#endif
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    static bool MEGLER_CB presentGpuCanvas(void *user, uint32_t texture, int width, int height);
    static bool MEGLER_CB releaseGpuCanvas(void *user, uint32_t texture);
    static bool MEGLER_CB presentRemoteGpuCanvas(void *user, uint32_t texture, int width, int height);
    static bool MEGLER_CB releaseRemoteGpuCanvas(void *user, uint32_t texture);
#endif
    static void MEGLER_CB appExited(void *user, int status, const char *reason);
    static void MEGLER_CB orientationRequested(void *user, int requested);
    void updateOrientationMask();
    void setStatus(const QString &status);
    void setFrameCaptureStatus(const QString &status);

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
    QString m_frameCaptureStatus;
    quint64 m_frameCaptureSequence = 0;
    bool m_frameCapturePending = false;
    bool m_frameCaptureEnabled = false;
    QAtomicInt m_frameCaptureResultQueued = 0;
    mutable QMutex m_frameMutex;
    QImage m_frame;
    QImage m_gestureFrame;
    bool m_framePending = false;
    bool m_awaitingGestureFrame = false;
    bool m_gestureFrameChanged = false;
    bool m_running = false;
    bool m_starting = false;
    bool m_sharedMicrogEnabled = false;
    std::unique_ptr<SharedMicrogClientChannel> m_sharedMicrog;
    int m_exitStatus = 0;
    bool m_closing = false;
    bool m_backgrounded = false;
    bool m_backgroundFinished = false;
    bool m_closeOnExit = false;
    bool m_closeToBackground = false;
    bool m_closeOnBackGesture = false;
    bool m_cpuPresentationOnly = false;
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    PreviewRemoteGpuCanvasPresent m_remoteGpuCanvasPresent = nullptr;
    EmbeddedGpuTransport *m_remoteGpuTransport = nullptr;
    void *m_remoteGpuCanvasUser = nullptr;
    QAtomicInt m_remoteGpuCanvasFailed = 0;
#endif
    bool m_hasRemoteKeyboard = false;
    megler_keyboard_host m_remoteKeyboard = {};
    int m_backGestureEdgeDp = 32;
    int m_surfaceWidth = 480;
    int m_surfaceHeight = 800;
    int m_density = 160;
    int m_displayRotation = 0;
    int m_requestedOrientation = MEGLER_ORIENTATION_UNSPECIFIED;
    int m_naturalOrientation = 0;
    int m_actualOrientation = 0;
    int m_orientationMask = -1;
    QPointer<PreviewKeyboard> m_keyboard;
    QPointer<PreviewSurface> m_surface;
    QTimer *m_backgroundMonitor = nullptr;
    AuroraStorageHost m_storage;
    AuroraAudioOutput *m_audio = nullptr;
    AuroraPickerHost *m_picker = nullptr;
    InstallRequestHost *m_installHost = nullptr;
    megler_system_host m_systemHost = {};
    NotificationHost *m_notificationHost = nullptr;
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
    Q_PROPERTY(int pageOrientation READ pageOrientation WRITE setPageOrientation)

public:
    explicit PreviewSurface(QQuickItem *parent = nullptr);
    ~PreviewSurface() override;
    QObject *player() const { return m_player; }
    void setPlayer(QObject *player);
    int pageOrientation() const { return m_pageOrientation; }
    void setPageOrientation(int orientation);
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
    void scheduleViewport();
    void updateViewport();
    int displayRotation();
    QPointer<PreviewPlayer> m_player;
    int m_pageOrientation = 0;
    int m_loggedNative = -2;
    int m_loggedActual = -2;
    int m_loggedPrimary = -2;
    int m_loggedRotation = -2;
    bool m_pressed = false;
    bool m_backPressed = false;
    qreal m_edgeZoneWidth = 32;
    QPointF m_lastPoint;
    QTimer *m_wheelTimer;
    QTimer *m_viewportTimer;
    QPointF m_wheelPoint, m_wheelStart, m_wheelEnd, m_wheelLast;
    QPointF m_wheelPending;
    int m_wheelStep = 0;
    bool m_wheelActive = false;
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
    PreviewGpuItem *m_gpuItem = nullptr;
#endif
};

#endif
