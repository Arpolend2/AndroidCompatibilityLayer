#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_IPC_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_IPC_H

#include <QDBusConnection>
#include <QDBusPendingReply>
#include <QDBusVirtualObject>
#include <QString>
#include <functional>

class SharedMicrogProviderObject : public QDBusVirtualObject
{
public:
    explicit SharedMicrogProviderObject(const QDBusConnection &bus, QObject *parent = nullptr,
                                       const std::function<void()> &onRequest = {});
    ~SharedMicrogProviderObject() override;

    bool start(QString *error);
    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

private:
    QDBusConnection m_bus;
    bool m_registered = false;
    int m_pendingIdentifications = 0;
    std::function<void()> m_onRequest;
};

QDBusPendingReply<QString> requestSharedMicrogProvider(const QDBusConnection &bus, bool autoStart = true);
bool readSharedMicrogProvider(const QDBusPendingReply<QString> &reply, QString *provider, QString *error);

#endif
