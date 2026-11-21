#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDebug>
#include <QTextStream>
#include <QUuid>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() > 2 || (args.size() == 2 && args.at(1) != QLatin1String("--invalid"))) {
        qCritical() << "Usage: client [--invalid]";
        return 1;
    }
    const bool invalid = args.size() == 2;
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "Session bus unavailable:" << bus.lastError().message();
        return 1;
    }
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
        QStringLiteral("Probe"));
    if (invalid) request << quint32(1) << bus.baseService();
    else request << quint32(1);
    const QDBusMessage reply = bus.call(request, QDBus::Block, 10000);
    if (invalid) {
        if (reply.type() == QDBusMessage::ErrorMessage &&
            reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.InvalidArgs")) return 0;
        qCritical() << "Expected invalid arguments error, got" << reply;
        return 1;
    }
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCritical() << reply.errorName() << reply.errorMessage();
        return 1;
    }
    const QList<QVariant> values = reply.arguments();
    if (reply.type() != QDBusMessage::ReplyMessage || values.size() != 4 ||
        values.at(0).type() != QVariant::UInt || values.at(0).toUInt() != 1 ||
        values.at(1).type() != QVariant::String ||
        values.at(1).toString() != QLatin1String("microg-test-ok") ||
        values.at(2).type() != QVariant::String || values.at(2).toString() != bus.baseService() ||
        values.at(3).type() != QVariant::String || QUuid(values.at(3).toString()).isNull()) {
        qCritical() << "Malformed probe response:" << reply;
        return 1;
    }
    QTextStream(stdout) << "PASS sender=" << values.at(2).toString()
                        << " instance=" << values.at(3).toString() << '\n';
    return 0;
}
