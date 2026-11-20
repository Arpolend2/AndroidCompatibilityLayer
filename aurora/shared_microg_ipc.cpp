#include "shared_microg_ipc.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QSettings>

namespace {
const char serviceName[] = "ru.androidcompatibilitylayer.appmanager";
const char objectPath[] = "/ru/androidcompatibilitylayer/appmanager/microg";
const char interfaceName[] = "ru.androidcompatibilitylayer.appmanager.microg";
const int maxPendingIdentifications = 16;

bool validProvider(const QString &provider)
{
    return provider == QLatin1String("none") || provider == QLatin1String("microg");
}

void sendIdentificationReply(const QDBusConnection &connection, const QDBusMessage &reply)
{
    if (!connection.send(reply))
        qWarning("Shared microG: could not send identification reply: %s",
                 qPrintable(connection.lastError().message()));
}
}

SharedMicrogProviderObject::SharedMicrogProviderObject(const QDBusConnection &bus, QObject *parent,
                                                       const std::function<void()> &onRequest)
    : QDBusVirtualObject(parent), m_bus(bus), m_onRequest(onRequest)
{
}

SharedMicrogProviderObject::~SharedMicrogProviderObject()
{
    if (m_registered)
        m_bus.unregisterObject(QString::fromLatin1(objectPath));
}

bool SharedMicrogProviderObject::start(QString *error)
{
    if (error) error->clear();
    if (m_registered) {
        if (error) *error = QStringLiteral("The shared microG provider object is already registered.");
        return false;
    }
    if (!m_bus.isConnected() || !m_bus.interface()) {
        if (error) *error = QStringLiteral("Session D-Bus is unavailable.");
        return false;
    }
    const QDBusReply<QString> owner = m_bus.interface()->serviceOwner(QString::fromLatin1(serviceName));
    if (!owner.isValid() || owner.value() != m_bus.baseService()) {
        if (error) *error = QStringLiteral("This process does not own the manager D-Bus service.");
        return false;
    }
    if (!m_bus.registerVirtualObject(QString::fromLatin1(objectPath), this, QDBusConnection::SingleNode)) {
        if (error) *error = m_bus.lastError().message();
        return false;
    }
    m_registered = true;
    return true;
}

QString SharedMicrogProviderObject::introspect(const QString &path) const
{
    if (path != QLatin1String(objectPath))
        return QString();
    return QStringLiteral("<interface name=\"ru.androidcompatibilitylayer.appmanager.microg\">"
                          "<method name=\"GetProvider\"><arg type=\"s\" direction=\"out\"/>"
                          "</method>"
                          "<method name=\"InspectCaller\"><arg type=\"as\" direction=\"out\"/>"
                          "</method></interface>");
}

bool SharedMicrogProviderObject::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    if (message.type() != QDBusMessage::MethodCallMessage ||
        message.path() != QLatin1String(objectPath) ||
        message.interface() != QLatin1String(interfaceName) ||
        (message.member() != QLatin1String("GetProvider") &&
         message.member() != QLatin1String("InspectCaller")))
        return false;
    if (!message.arguments().isEmpty()) {
        connection.send(message.createErrorReply(QDBusError::InvalidArgs,
                                                QStringLiteral("This method accepts no arguments.")));
        return true;
    }
    if (message.member() == QLatin1String("InspectCaller")) {
        const QString sender = message.service();
        if (!sender.startsWith(QLatin1Char(':'))) {
            sendIdentificationReply(connection, message.createErrorReply(QDBusError::AccessDenied,
                                                     QStringLiteral("A D-Bus sender is required.")));
            return true;
        }
        if (m_pendingIdentifications >= maxPendingIdentifications) {
            sendIdentificationReply(connection, message.createErrorReply(QDBusError::LimitsExceeded,
                                                     QStringLiteral("Too many identification requests.")));
            return true;
        }
        if (m_onRequest) m_onRequest();
        QDBusMessage identify = QDBusMessage::createMethodCall(
            QStringLiteral("ru.omp.am"), QStringLiteral("/ru/omp/am"),
            QStringLiteral("ru.omp.am.Identify"), QStringLiteral("IdentifyByBusName"));
        identify << sender;
        ++m_pendingIdentifications;
        message.setDelayedReply(true);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            connection.asyncCall(identify, 5000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, connection, message, sender]() {
            --m_pendingIdentifications;
            const QDBusMessage reply = watcher->reply();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                sendIdentificationReply(connection, message.createErrorReply(reply.errorName(),
                    QStringLiteral("IdentifyByBusName failed: ") + reply.errorMessage()));
            } else {
                const QList<QVariant> values = reply.arguments();
                if (reply.type() != QDBusMessage::ReplyMessage || values.size() != 2 ||
                    values.at(0).type() != QVariant::String ||
                    values.at(1).type() != QVariant::String ||
                    values.at(0).toString().isEmpty() || values.at(0).toString().size() > 4096 ||
                    values.at(1).toString().size() > 256) {
                    sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed,
                        QStringLiteral("Invalid IdentifyByBusName response.")));
                } else {
                    sendIdentificationReply(connection, message.createReply(QVariantList() <<
                        (QStringList() << sender << values.at(0).toString() << values.at(1).toString())));
                }
            }
            watcher->deleteLater();
        });
        return true;
    }
    if (m_onRequest) m_onRequest();
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        connection.send(message.createErrorReply(QDBusError::Failed,
                                                QStringLiteral("Cannot read the manager settings.")));
        return true;
    }
    const QString provider = settings.value(QStringLiteral("googleServices/provider"),
                                            QStringLiteral("none")).toString();
    if (settings.status() != QSettings::NoError || !validProvider(provider)) {
        connection.send(message.createErrorReply(QDBusError::Failed,
                                                QStringLiteral("Invalid Google services provider setting.")));
        return true;
    }
    connection.send(message.createReply(QVariantList() << provider));
    return true;
}

QDBusPendingReply<QString> requestSharedMicrogProvider(const QDBusConnection &bus, bool autoStart)
{
    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(serviceName), QString::fromLatin1(objectPath),
        QString::fromLatin1(interfaceName), QStringLiteral("GetProvider"));
    request.setAutoStartService(autoStart);
    return bus.asyncCall(request, 15000);
}

bool readSharedMicrogProvider(const QDBusPendingReply<QString> &reply, QString *provider, QString *error)
{
    if (provider) provider->clear();
    if (error) error->clear();
    if (!reply.isFinished()) {
        if (error) *error = QStringLiteral("The provider request has not finished.");
        return false;
    }
    if (reply.isError()) {
        if (error) *error = reply.error().name() + QStringLiteral(": ") + reply.error().message();
        return false;
    }
    if (!validProvider(reply.value())) {
        if (error) *error = QStringLiteral("The manager returned an invalid provider.");
        return false;
    }
    if (provider) *provider = reply.value();
    return true;
}

