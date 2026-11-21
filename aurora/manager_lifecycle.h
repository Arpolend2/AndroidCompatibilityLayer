#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_MANAGER_LIFECYCLE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_MANAGER_LIFECYCLE_H

#include "external_apk_receiver.h"
#include "shared_microg_ipc.h"
#include "shared_microg_supervisor.h"

#include <QTimer>
#include <functional>

class ManagerLifecycle : public QObject
{
    Q_OBJECT
public:
    enum ForwardResult { NoOwner, Delivered, Unavailable, Failed };

    explicit ManagerLifecycle(const std::function<bool()> &showGui, QObject *parent = nullptr,
                              int idleMilliseconds = 60000);
    ~ManagerLifecycle() override;
    bool start(bool background, QString *error);
    bool configureSharedMicrogWorker(const QString &program, const QStringList &arguments,
                                     const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                     QString *error);
    void disableSharedMicrogWorker();
    static SharedMicrogProviderObject::PackageResolver registryPackageResolver(
        const QString &identityDirectory);
    void setKeepAlive(bool keepAlive);
    void refreshSharedMicrogProvider() { m_provider.refreshProviderSetting(); }
    bool submit(const QStringList &arguments) { return m_receiver.submit(arguments); }
    bool recordStoreResult(const QString &path, bool installed, const QByteArray &fingerprint)
    { return m_receiver.recordStoreResult(path, installed, fingerprint); }
    void setLibraryApps(const QString &contents) { m_receiver.setLibraryApps(contents); }
    void setLaunchHandler(const std::function<void(const QString &, const std::function<void(const QString &)> &)> &handler)
    { m_receiver.setLaunchHandler(handler); }
    static QStringList launchFileArguments(const QStringList &arguments);
    static ForwardResult forwardToRunningManager(const QStringList &arguments, QString *error);

signals:
    void fileRequested(const QString &path);
    void fileRejected(const QString &message);
    void standaloneLaunchFailed(const QString &package, const QString &message);

private:
    bool showGui();
    void noteRequest();

    std::function<bool()> m_showGui;
    bool m_background = false;
    bool m_keepAlive = false;
    QTimer m_idle;
    ExternalApkReceiver m_receiver;
    SharedMicrogSupervisor m_worker;
    SharedMicrogProviderObject m_provider;
};

#endif
