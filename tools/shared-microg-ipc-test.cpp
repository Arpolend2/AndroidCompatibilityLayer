#include "../aurora/shared_microg_ipc.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {
const char service[] = "ru.androidcompatibilitylayer.appmanager";
const char path[] = "/ru/androidcompatibilitylayer/appmanager/microg";
const char interface[] = "ru.androidcompatibilitylayer.appmanager.microg";

class IdentityFixture : public QDBusVirtualObject
{
public:
    QStringList seen;
    bool malformed = false;
    int delayMs = 0;

    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"ru.omp.am.Identify\">"
            "<method name=\"IdentifyByBusName\"><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
            "</method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("ru.omp.am.Identify") ||
            message.member() != QLatin1String("IdentifyByBusName")) return false;
        if (message.arguments().size() != 1 || message.arguments().first().type() != QVariant::String) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad arguments")));
            return true;
        }
        seen.append(message.arguments().first().toString());
        const QVariantList result = malformed
            ? QVariantList() << 123 << QStringLiteral("key")
            : QVariantList() << QStringLiteral("/test/client") << QStringLiteral("test-key");
        QTimer::singleShot(delayMs, this, [message, connection, result]() {
            connection.send(message.createReply(result));
        });
        return true;
    }
};

void check(bool value, const char *description)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

QDBusPendingCall waitFor(const QDBusPendingCall &call)
{
    QDBusPendingCallWatcher watcher(call);
    if (!watcher.isFinished()) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(8000);
        loop.exec();
    }
    check(watcher.isFinished(), "D-Bus request timed out");
    return watcher;
}

QDBusPendingReply<QString> request(const QDBusConnection &bus)
{
    return QDBusPendingReply<QString>(waitFor(requestSharedMicrogProvider(bus, false)));
}

QDBusPendingCall inspect(const QDBusConnection &bus, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("InspectCaller"));
    message.setArguments(arguments);
    return bus.asyncCall(message, 6000);
}

void setProvider(const QString &value)
{
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), value);
    settings.sync();
    check(settings.status() == QSettings::NoError, "write isolated settings");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    QTemporaryDir temporary;
    check(temporary.isValid(), "create isolated settings directory");
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
    QDBusConnection bus = QDBusConnection::sessionBus();
    check(bus.isConnected(), "connect to session bus");

    QString provider, error;
    check(!readSharedMicrogProvider(request(bus), &provider, &error) && !error.isEmpty(),
          "unavailable manager reports an error");
    const QString marker = QString::fromLocal8Bit(qgetenv("ACL_AUTOSTART_MARKER"));
    check(!marker.isEmpty() && !QFile::exists(marker), "status request does not activate service");
    check(!bus.interface()->isServiceRegistered(QString::fromLatin1(service)).value(),
          "status request does not start the manager");
    {
        SharedMicrogProviderObject notOwner(bus);
        check(!notOwner.start(&error) && !error.isEmpty(), "object requires manager name owner");
    }

    check(bus.registerService(QString::fromLatin1(service)), "own manager name for test");
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                         QStringLiteral("microg-test-other"));
    check(other.isConnected(), "connect second bus client");
    {
        SharedMicrogProviderObject notOwner(other);
        check(!notOwner.start(&error) && !error.isEmpty(), "other connection cannot register service object");
    }
    {
        SharedMicrogProviderObject object(bus);
        check(object.start(&error), "register provider object");
        check(!object.start(&error) && !error.isEmpty(), "reject duplicate object registration");
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("none"), "default provider is none");
        setProvider(QStringLiteral("microg"));
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("microg"), "read microg from manager settings");
        setProvider(QStringLiteral("none"));
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("none"), "read updated provider");
        setProvider(QStringLiteral("invalid"));
        check(!readSharedMicrogProvider(request(other), &provider, &error) && !error.isEmpty(),
              "unknown provider returns an error");

        QDBusMessage invalid = QDBusMessage::createMethodCall(
            QString::fromLatin1(service), QString::fromLatin1(path),
            QString::fromLatin1(interface), QStringLiteral("GetProvider"));
        invalid << QStringLiteral("unexpected");
        QDBusPendingReply<QString> invalidReply(waitFor(other.asyncCall(invalid)));
        check(invalidReply.isError() &&
              invalidReply.error().type() == QDBusError::InvalidArgs, "reject unexpected arguments");

        QDBusPendingReply<QStringList> failedIdentity(waitFor(inspect(other)));
        check(failedIdentity.isError(), "unavailable identity service returns an error");
        IdentityFixture identity;
        QDBusConnection identityBus = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-identity"));
        QDBusConnection other2 = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-other2"));
        check(identityBus.isConnected() && other2.isConnected(), "connect identity fixture and second client");
        check(identityBus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity),
              "register identity test object");
        check(identityBus.registerService(QStringLiteral("ru.omp.am")), "register identity test service");
        QDBusPendingReply<QStringList> identityReply(waitFor(inspect(other)));
        check(!identityReply.isError() &&
              identityReply.value() == (QStringList() << other.baseService()
                                        << QStringLiteral("/test/client") << QStringLiteral("test-key")) &&
              identity.seen.last() == other.baseService(),
              "identify the real sender of the other connection");
        QDBusPendingReply<QStringList> identityReply2(waitFor(inspect(other2)));
        check(!identityReply2.isError() && identityReply2.value().first() == other2.baseService() &&
              identity.seen.last() == other2.baseService() && other2.baseService() != other.baseService(),
              "identify distinct D-Bus connections independently");
        const int beforeSpoof = identity.seen.size();
        QDBusPendingReply<QStringList> spoof(waitFor(inspect(other, QVariantList() << bus.baseService())));
        check(spoof.isError() && spoof.error().type() == QDBusError::InvalidArgs &&
              identity.seen.size() == beforeSpoof, "reject a caller-supplied identity");
        identity.malformed = true;
        QDBusPendingReply<QStringList> malformed(waitFor(inspect(other)));
        check(malformed.isError(), "reject malformed identification response");
        identity.malformed = false;
        identity.delayMs = 300;
        QList<QDBusPendingCall> concurrent;
        for (int i = 0; i < 17; ++i) concurrent.append(inspect(other));
        QDBusPendingReply<QStringList> limited(waitFor(concurrent.last()));
        check(limited.isError() && limited.error().type() == QDBusError::LimitsExceeded,
              "limit concurrent identification requests");
        for (int i = 0; i < 16; ++i) {
            QDBusPendingReply<QStringList> completed(waitFor(concurrent.at(i)));
            check(!completed.isError(), "in-flight identification completes");
        }
        identity.delayMs = 0;
        QDBusPendingReply<QStringList> afterLimit(waitFor(inspect(other)));
        check(!afterLimit.isError(), "identification capacity is released");
        identity.delayMs = 5500;
        QDBusPendingReply<QStringList> timedOut(waitFor(inspect(other)));
        check(timedOut.isError(), "identity service timeout returns an error");
        identity.delayMs = 0;
        QDBusPendingReply<QStringList> afterTimeout(waitFor(inspect(other)));
        check(!afterTimeout.isError(), "identification capacity is released after timeout");
        identityBus.unregisterService(QStringLiteral("ru.omp.am"));
        identityBus.unregisterObject(QStringLiteral("/ru/omp/am"));
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-identity"));
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-other2"));

        const QString settingsPath = temporary.path() + QStringLiteral("/bad-settings");
        check(QDir().mkpath(settingsPath + QStringLiteral("/ru.androidcompatibilitylayer")),
              "create invalid settings directory");
        QFile file(settingsPath + QStringLiteral("/ru.androidcompatibilitylayer/appmanager.conf"));
        check(file.open(QIODevice::WriteOnly), "create malformed settings");
        check(file.write("[invalid\nkey=value\n") > 0, "write malformed settings");
        file.close();
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsPath);
        check(!readSharedMicrogProvider(request(other), &provider, &error) && !error.isEmpty(),
              "settings read failure returns an error");
    }
    check(bus.unregisterService(QString::fromLatin1(service)), "release manager name");
    QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-other"));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("GetProvider"));
    QDBusMessage badResponse = call.createReply(QVariantList() << QStringLiteral("unexpected"));
    QDBusPendingReply<QString> badReply(QDBusPendingCall::fromCompletedCall(badResponse));
    check(!readSharedMicrogProvider(badReply, &provider, &error) && !error.isEmpty(),
          "reject malformed provider in reply");
    check(!QFile::exists(marker), "manager activation did not run");
    bus.interface()->startService(QString::fromLatin1(service));
    check(QFile::exists(marker), "activation fixture is discoverable by the bus");
    std::puts("shared microG IPC tests passed");
    return 0;
}
