#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_IPC_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_IPC_H

#include <QDBusConnection>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QHash>
#include <QString>
#include <QTimer>
#include <functional>

class QDBusServiceWatcher;

class SharedMicrogProviderObject : public QDBusVirtualObject
{
public:
    // Runs on the provider's thread. The trusted backend must duplicate or retain
    // the server descriptor and release the channel when its bridge terminates.
    using ChannelHandler = std::function<bool(const QString &sender, const QString &package,
                                               const QByteArray &appMetadata,
                                               int uid, int pid, quint64 channelId,
                                               const QDBusUnixFileDescriptor &server, QString *error)>;
    using ChannelReleaseHandler = std::function<void(const QString &sender, quint64 channelId)>;
    using PackageResolver = std::function<QString(const QString &executable,
                                                   const QString &identityKey, QString *error)>;
    using ApkMetadataResolver = std::function<QByteArray(const QString &apkPath,
                                                         const QString &expectedPackage,
                                                         QString *error)>;
    explicit SharedMicrogProviderObject(const QDBusConnection &bus, QObject *parent = nullptr,
                                       const std::function<void()> &onRequest = {},
                                       const QString &identityDirectory = QString(),
                                       const ChannelHandler &channelHandler = {},
                                       const PackageResolver &packageResolver = {});
    ~SharedMicrogProviderObject() override;

    bool start(QString *error);
    bool setChannelHandlers(const ChannelHandler &channelHandler,
                            const ChannelReleaseHandler &releaseHandler, QString *error);
    bool setPackageResolver(const PackageResolver &packageResolver, QString *error);
    bool setApkMetadataResolver(const ApkMetadataResolver &metadataResolver, QString *error);
    void setWorkerCheckHandler(const std::function<bool(QString *)> &handler)
    { m_workerCheckHandler = handler; }
    bool clearBackend(QString *error);
    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;
    // Invoke on the provider's thread; stale channel IDs cannot revoke a replacement.
    void releaseChannel(const QString &sender, quint64 channelId);
    void releaseChannelById(quint64 channelId);
    void revokeAllChannels();
    void refreshProviderSetting();

private:
    struct Channel {
        quint64 id;
        QDBusUnixFileDescriptor server;
        bool workerAccepted;
    };
    void revokeChannel(const QString &sender);
    void openChannel(const QDBusMessage &message, const QDBusConnection &connection,
                     const QString &sender, const QString &package, const QByteArray &appMetadata,
                     int uid, int pid,
                     bool withIdentity);

    QDBusConnection m_bus;
    bool m_registered = false;
    int m_pendingIdentifications = 0;
    int m_nextVirtualPid = 50000;
    QHash<QString, int> m_virtualPids;
    quint64 m_nextChannelId = 1;
    QHash<QString, Channel> m_channels;
    QTimer m_providerCheck;
    QDBusServiceWatcher *m_senderWatcher;
    QString m_identityDirectory;
    std::function<void()> m_onRequest;
    ChannelHandler m_channelHandler;
    ChannelReleaseHandler m_channelReleaseHandler;
    PackageResolver m_packageResolver;
    ApkMetadataResolver m_apkMetadataResolver;
    std::function<bool(QString *)> m_workerCheckHandler;
};

QDBusPendingReply<QString> requestSharedMicrogProvider(const QDBusConnection &bus, bool autoStart = true);
bool readSharedMicrogProvider(const QDBusPendingReply<QString> &reply, QString *provider, QString *error);

// Transport only: the caller must authenticate and authorize the peer separately.
bool createSharedMicrogStreamPair(QDBusUnixFileDescriptor *local, QDBusUnixFileDescriptor *peer,
                                  QString *error);
int takeSharedMicrogStream(QDBusUnixFileDescriptor *descriptor, QString *error);

// Allocate stable guest UIDs for manager-validated packages. This does not authenticate a caller.
bool sharedMicrogUidForPackage(const QString &directory, const QString &package,
                              int *uid, QString *error);

#endif
