#ifndef ANDROID_COMPATIBILITY_LAYER_EMBEDDED_PREVIEW_H
#define ANDROID_COMPATIBILITY_LAYER_EMBEDDED_PREVIEW_H

#include "preview.h"
#include "preview_sessions.h"
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include "embedded_gpu_transport.h"
#endif

#include <QAtomicInt>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

class QProcess;
class PreviewKeyboard;
class AuroraPickerHost;

class EmbeddedChannel : public QObject
{
    Q_OBJECT
public:
    explicit EmbeddedChannel(QObject *parent = nullptr);
    ~EmbeddedChannel() override;
    void attach(QLocalSocket *socket);
    void reset();
    bool send(const QJsonObject &message);
    bool sendFrame(const QImage &frame);
    bool connected() const;
    /* Frames through a shared mapping instead of the socket: the child writes each frame into the
       file at `path` and sends only its size and generation, so no pixels cross the socket. Both
       ends call this with the same path (the preview channel's directory, "<dir>/frame"). Empty
       disables it and the frames go over the socket as before. */
    void useSharedFrames(const QString &path);
signals:
    void message(const QJsonObject &message);
    void frame(const QImage &frame);
    void failed(const QString &reason);
private:
    void read();
    void fail(const QString &reason);
    QPointer<QLocalSocket> m_socket;
    QByteArray m_input;
    bool m_failed = false;
    bool ensureShared(qint64 bytes, bool writable);
    void closeShared();
    QString m_sharedPath;
    int m_sharedFd = -1;
    uchar *m_sharedMap = nullptr;
    qint64 m_sharedBytes = 0;
    bool m_sharedWritable = false;
    quint32 m_sharedGeneration = 0;
    bool m_sharedLogged = false;
};

class EmbeddedPreview : public PreviewPlayer, public EmbeddedEndpoint
{
    Q_OBJECT
    Q_PROPERTY(QString status READ remoteStatus NOTIFY remoteStatusChanged)
    Q_PROPERTY(bool showStatus READ remoteShowStatus NOTIFY remoteStatusChanged)
public:
    explicit EmbeddedPreview(QObject *parent = nullptr);
    bool listen(QProcess *process, QString *error) override;
    QString endpoint() const override;
    bool stopping() const override { return m_stopping; }
    bool usesGpuSurface() const override
    {
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
        return true;
#else
        return false;
#endif
    }
    void setRemoteKeyboard(PreviewKeyboard *keyboard);
    Q_INVOKABLE void setRemoteTheme(bool dark, const QColor &accent);
    QString remoteStatus() const { return m_status; }
    bool remoteShowStatus() const { return !m_status.isEmpty(); }
    QImage frame() const override { return m_frame; }
    bool touch(int phase, float x, float y) override;
    void back() override;
    void setViewport(qreal width, qreal height, qreal ratio) override;
    void expectRestart();
    Q_INVOKABLE void close() override;
    Q_INVOKABLE void closeToManager() override;
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setUiReady(bool ready);
    Q_INVOKABLE void selected(const QString &token, const QString &path);
    Q_INVOKABLE void canceled(const QString &token);
    Q_INVOKABLE void showFailed(const QString &token);
signals:
    void remoteStatusChanged();
    void showPicker(const QString &token, const QStringList &filters);
    void channelError(const QString &reason);
    void shutdownComplete();
private:
    void acceptPending();
    void receive(const QJsonObject &message);
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    void receiveGpuFrame(AuroraDmaBufFrame &&frame);
#endif
    void command(const QString &type, const QJsonObject &values = QJsonObject());
    QTemporaryDir m_directory;
    QLocalServer m_server;
    QTimer m_connectionTimer;
    EmbeddedChannel m_channel;
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    EmbeddedGpuTransport m_gpuTransport;
    QString m_gpuEndpoint;
#endif
    QPointer<QProcess> m_process;
    QPointer<PreviewKeyboard> m_keyboard;
    QImage m_frame;
    QString m_status;
    qreal m_width = 0, m_height = 0, m_ratio = 1;
    bool m_stopping = false;
    bool m_restarting = false;
    bool m_ready = false;
    bool m_dark = false;
    QColor m_accent;
};

class EmbeddedPreviewChild : public QObject
{
    Q_OBJECT
public:
    EmbeddedPreviewChild(PreviewPlayer *player, AuroraPickerHost *picker, QObject *parent = nullptr);
    bool connectTo(const QString &endpoint, QString *error);
    void notifyShutdownComplete();
    megler_keyboard_host keyboardHost();
signals:
    void ready();
    void failed(const QString &reason);
private slots:
    void showKeyboard(int kind, int flags, int enter);
    void hideKeyboard();
private:
    static void MEGLER_CB requestShow(void *user, int kind, int flags, int enter);
    static void MEGLER_CB requestHide(void *user);
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    static bool MEGLER_CB presentGpuCanvas(void *user, uint32_t texture, int width, int height);
#endif
    void receive(const QJsonObject &message);
    void sendFrame();
    void sendStatus();
    void maybeStart();
    void command(const QString &type, const QJsonObject &values = QJsonObject());
    PreviewPlayer *m_player;
    AuroraPickerHost *m_picker;
    QLocalSocket m_socket;
    EmbeddedChannel m_channel;
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    EmbeddedGpuTransport m_gpuTransport;
    QAtomicInt m_gpuPresenting = 0;
    QAtomicInt m_gpuFailed = 0;
    QAtomicInt m_gpuActiveLogged = 0;
#endif
    bool m_frameOutstanding = false;
    bool m_framePending = false;
    bool m_statusSent = false;
    bool m_uiReady = false;
    bool m_haveViewport = false;
    bool m_startScheduled = false;
    bool m_stopping = false;
    QString m_lastStatus;
};

#endif
