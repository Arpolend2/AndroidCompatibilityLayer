#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QTimer>
#include <QUuid>

namespace {
const char serviceName[] = "ru.androidcompatibilitylayer.appmanager";
const char objectPath[] = "/ru/androidcompatibilitylayer/appmanager/microg";
const char interfaceName[] = "ru.androidcompatibilitylayer.appmanager.microg";

class Probe : public QDBusVirtualObject
{
public:
    Probe() : instance(QUuid::createUuid().toString())
    {
        idle.setSingleShot(true);
        QObject::connect(&idle, &QTimer::timeout, QCoreApplication::instance(), &QCoreApplication::quit);
        idle.start(60000);
    }

    QString introspect(const QString &path) const override
    {
        if (path != QLatin1String(objectPath)) return QString();
        return QStringLiteral("<interface name=\"ru.androidcompatibilitylayer.appmanager.microg\">"
                              "<method name=\"Probe\"><arg type=\"u\" direction=\"in\"/>"
                              "<arg type=\"u\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
                              "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
                              "</method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.type() != QDBusMessage::MethodCallMessage ||
            message.path() != QLatin1String(objectPath) ||
            message.interface() != QLatin1String(interfaceName) ||
            message.member() != QLatin1String("Probe")) return false;
        const QList<QVariant> arguments = message.arguments();
        if (arguments.size() != 1 || arguments.first().type() != QVariant::UInt ||
            arguments.first().toUInt() != 1) {
            if (!connection.send(message.createErrorReply(QDBusError::InvalidArgs,
                    QStringLiteral("Expected protocol version 1 as uint32"))))
                qWarning() << "Cannot send probe error:" << connection.lastError().message();
            return true;
        }
        const QString sender = message.service();
        if (!sender.startsWith(QLatin1Char(':'))) {
            if (!connection.send(message.createErrorReply(QDBusError::AccessDenied,
                    QStringLiteral("Missing D-Bus sender"))))
                qWarning() << "Cannot send probe error:" << connection.lastError().message();
            return true;
        }
        idle.start();
        if (!connection.send(message.createReply(QVariantList() << quint32(1)
                << QStringLiteral("microg-test-ok") << sender << instance)))
            qWarning() << "Cannot send probe reply:" << connection.lastError().message();
        return true;
    }

private:
    const QString instance;
    QTimer idle;
};
}

Q_DECL_EXPORT int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qgetenv("ACL_MICROG_ISOLATED_TEST") != "1") {
        qCritical() << "This test fixture must only be run on an isolated test bus.";
        return 1;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "Session bus unavailable:" << bus.lastError().message();
        return 1;
    }
    Probe probe;
    if (!bus.registerVirtualObject(QString::fromLatin1(objectPath), &probe) ||
        !bus.registerService(QString::fromLatin1(serviceName))) {
        qCritical() << "Cannot register probe:" << bus.lastError().message();
        return 1;
    }
    return app.exec();
}
