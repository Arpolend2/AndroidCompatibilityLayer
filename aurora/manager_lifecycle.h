#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_MANAGER_LIFECYCLE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_MANAGER_LIFECYCLE_H

#include "external_apk_receiver.h"
#include "shared_microg_ipc.h"
#include "shared_microg_supervisor.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QSet>
#include <QTimer>
#include <functional>

class PreviewKeyboard;
class QMouseEvent;
class QPainter;
class QQuickItem;
class QRectF;

class ManagerLifecycle : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool authenticationUiActive READ authenticationUiActive NOTIFY authenticationUiChanged)
    Q_PROPERTY(bool authenticationFrameAvailable READ authenticationFrameAvailable NOTIFY authenticationFrameChanged)
    Q_PROPERTY(QImage authenticationFrame READ authenticationFrame NOTIFY authenticationFrameChanged)
public:
    enum ForwardResult { NoOwner, Delivered, Unavailable, Failed };

    explicit ManagerLifecycle(const std::function<bool()> &showGui, QObject *parent = nullptr,
                              int idleMilliseconds = 60000);
    ~ManagerLifecycle() override;
    bool start(bool background, QString *error);
    bool configureSharedMicrogWorker(const QString &program, const QStringList &arguments,
                                     const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                     QString *error);
    bool configureSharedMicrogWorker(const QString &program, const QStringList &arguments,
                                     const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                     const SharedMicrogProviderObject::ApkMetadataResolver &metadataResolver,
                                     QString *error);
    void disableSharedMicrogWorker();
    void enableSharedMicrogBackground();
    static SharedMicrogProviderObject::PackageResolver registryPackageResolver(
        const QString &identityDirectory);
    void setKeepAlive(bool keepAlive);
    void refreshSharedMicrogProvider() { m_provider.refreshProviderSetting(); }
    bool submit(const QStringList &arguments) { return m_receiver.submit(arguments); }
    bool recordStoreResult(const QString &path, bool installed, const QByteArray &fingerprint)
    { return m_receiver.recordStoreResult(path, installed, fingerprint); }
    void setLibraryApps(const QString &contents) { m_receiver.setLibraryApps(contents); }
    bool authenticationUiActive() const { return m_authUiChannel != 0; }
    bool authenticationFrameAvailable() const { return !m_authFrame.isNull(); }
    QImage authenticationFrame() const { return m_authFrame; }
    void setAuthenticationKeyboard(PreviewKeyboard *keyboard);
    Q_INVOKABLE bool authenticationTouch(int phase, int x, int y);
    Q_INVOKABLE void authenticationBack();
    Q_INVOKABLE void cancelAuthentication();
    Q_INVOKABLE void setAuthenticationSurface(QObject *surface, int pixelWidth, int pixelHeight);
    void setAuthenticationViewport(const QSize &pixels, int density, int rotation);
    void invalidateAuthenticationViewport();
    void setLaunchHandler(const std::function<void(const QString &, const std::function<void(const QString &)> &)> &handler)
    { m_receiver.setLaunchHandler(handler); }
    static QStringList launchFileArguments(const QStringList &arguments);
    static ForwardResult forwardToRunningManager(const QStringList &arguments, QString *error);

signals:
    void fileRequested(const QString &path);
    void fileRejected(const QString &message);
    void standaloneLaunchFailed(const QString &package, const QString &message);
    void authenticationUiChanged();
    void authenticationFrameChanged();

private:
    struct ClientAppMetadata {
        QByteArray payload;
        QByteArray appsLine;
        QList<QByteArray> previousAppsLines;
        QByteArray previousActivities;
        bool hadAppsFile = false;
        bool hadActivities = false;
        QSet<quint64> channels;
    };
    bool showGui();
    void noteRequest();
    bool publishClientAppMetadata(const QString &package, const QByteArray &payload,
                                  quint64 channelId, QString *error);
    void releaseClientAppMetadata(quint64 channelId);
    bool writeClientAppsList(QString *error);
    void closeAuthenticationUi(quint64 channelId);
    void acceptAuthenticationFrame(quint64 channelId, const QImage &frame);
    bool sendAuthenticationText(const QString &text);
    bool sendAuthenticationTextKey(int key);
    void sendAuthenticationKeyboardClosed();
    void sendAuthenticationKeyboardHeight(int height);

    std::function<bool()> m_showGui;
    bool m_background = false;
    bool m_keepAlive = false;
    QTimer m_idle;
    QTimer m_microgRestart;
    bool m_microgBackground = false;
    ExternalApkReceiver m_receiver;
    SharedMicrogSupervisor m_worker;
    SharedMicrogProviderObject m_provider;
    QString m_appsDirectory;
    QHash<QString, ClientAppMetadata> m_clientAppMetadata;
    QHash<quint64, QString> m_clientAppChannels;
    quint64 m_authUiChannel = 0;
    QImage m_authFrame;
    QImage m_pendingAuthFrame;
    QSize m_authViewport;
    int m_authDensity = 0;
    int m_authRotation = 0;
    bool m_authFrameCurrent = false;
    bool m_authViewportPending = false;
    bool m_authAwaitingViewportFrame = false;
    QPointer<PreviewKeyboard> m_authKeyboard;
    QPointer<QQuickItem> m_authSurface;
};

class SharedMicrogAuthSurface : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *host READ host WRITE setHost NOTIFY hostChanged)
    Q_PROPERTY(int pageOrientation READ pageOrientation WRITE setPageOrientation)
public:
    explicit SharedMicrogAuthSurface(QQuickItem *parent = nullptr);
    QObject *host() const { return m_host; }
    void setHost(QObject *host);
    int pageOrientation() const { return m_pageOrientation; }
    void setPageOrientation(int orientation);
    void paint(QPainter *painter) override;

signals:
    void hostChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void scheduleViewport();
    void updateViewport();
    bool sendTouch(int phase, const QPointF &point);
    QRectF imageTarget(const QImage &frame) const;
    QPointer<ManagerLifecycle> m_host;
    QPointF m_lastTouch;
    bool m_pressed = false;
    int m_lastGuestX = 0;
    int m_lastGuestY = 0;
    int m_pageOrientation = int(Qt::PrimaryOrientation);
    int m_rotation = 0;
    QTimer m_viewportTimer;
    QMetaObject::Connection m_screenConnection;
};

#endif
