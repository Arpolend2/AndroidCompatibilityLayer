#include "../network_connman.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QTimer>
#include <cassert>

class ConnmanTestService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.connman.Manager")
public slots:
    ConnmanServices GetServices() const
    {
        ConnmanService service;
        service.path = QDBusObjectPath(QStringLiteral("/service/test"));
        service.properties.insert(QStringLiteral("State"), QStringLiteral("online"));
        service.properties.insert(QStringLiteral("Type"), QStringLiteral("wifi"));
        service.properties.insert(QStringLiteral("Favorite"), true);
        return ConnmanServices() << service;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDBusRegisterMetaType<ConnmanService>();
    qDBusRegisterMetaType<ConnmanServices>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    assert(bus.isConnected());
    const bool registeredService = bus.registerService(QStringLiteral("ru.megler.NetworkTest"));
    assert(registeredService);
    ConnmanTestService service;
    const bool registeredObject = bus.registerObject(QStringLiteral("/"), &service, QDBusConnection::ExportAllSlots);
    assert(registeredObject);
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("ru.megler.NetworkTest"), QStringLiteral("/"),
        QStringLiteral("net.connman.Manager"), QStringLiteral("GetServices"));
    QDBusPendingCallWatcher watcher(bus.asyncCall(message, 3000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(4000, &loop, &QEventLoop::quit);
    if (!watcher.isFinished()) loop.exec();
    assert(watcher.isFinished());
    QDBusPendingReply<ConnmanServices> reply = watcher;
    assert(!reply.isError());
    assert(reply.value().size() == 1);
    const ConnmanRoute route = connmanRoute(reply.value());
    assert(route.valid && route.online && route.favorite && route.type == QLatin1String("wifi"));
}

#include "network_connman_dbus_test.moc"
