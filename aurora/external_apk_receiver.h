#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_EXTERNAL_APK_RECEIVER_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_EXTERNAL_APK_RECEIVER_H

#include <QObject>
#include <QDateTime>
#include <QMap>
#include <QStringList>
#include <QDBusContext>
#include <QtDBus/qdbusmacros.h>
#include <functional>

class ExternalApkReceiver : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "ru.androidcompatibilitylayer.appmanager")
public:
    explicit ExternalApkReceiver(QObject *parent = nullptr);
    ~ExternalApkReceiver() override;
    bool start(QString *error);
    bool ownsLegacyService() const { return m_legacyRegistered; }
    void setActivationHandler(const std::function<bool()> &handler);
    bool submit(const QStringList &arguments);
    bool recordStoreResult(const QString &path, bool installed, const QByteArray &fingerprint);
    void setLibraryApps(const QString &contents) { m_libraryApps = contents; }
    void setLaunchHandler(const std::function<void(const QString &, const std::function<void(const QString &)> &)> &handler)
    { m_launchHandler = handler; }

public slots:
    Q_SCRIPTABLE Q_NOREPLY void openFile(const QStringList &arguments);
    Q_SCRIPTABLE bool activate(const QStringList &arguments);
    Q_SCRIPTABLE QString storeResult(const QString &id);
    Q_SCRIPTABLE QString libraryApps();
    Q_SCRIPTABLE QString launchApp(const QString &package);

signals:
    void requested(const QString &path);
    void rejected(const QString &message);

private:
    bool submitFrom(const QStringList &arguments, const char *source);
    bool m_legacyRegistered;
    bool m_runtimeStarted;
    std::function<bool()> m_activationHandler;
    std::function<void(const QString &, const std::function<void(const QString &)> &)> m_launchHandler;
    int m_launchPending = 0;
    struct StoreResult { QString value; qint64 time; bool reported; };
    QMap<QString, StoreResult> m_storeResults;
    QString m_libraryApps;
    bool m_storePollReported = false;
    bool m_libraryPollReported = false;
};

#endif
