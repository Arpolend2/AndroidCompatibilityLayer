#include "microg_watchdog.h"
#include "application_executables.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QSettings>
#include <QTimer>

namespace {
const char managerService[] = "ru.androidcompatibilitylayer.appmanager";

bool managerRunning(bool *running)
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) {
        qWarning("Cannot check the shared microG manager: session D-Bus is unavailable.");
        return false;
    }
    const QDBusReply<bool> reply = bus->isServiceRegistered(QString::fromLatin1(managerService));
    if (!reply.isValid()) {
        qWarning("Cannot check the shared microG manager: %s", qPrintable(reply.error().message()));
        return false;
    }
    *running = reply.value();
    return true;
}

bool ensureWorker()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(managerService),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
        QStringLiteral("EnsureWorker"));
    message.setAutoStartService(false);
    const QDBusReply<bool> reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 25000);
    if (!reply.isValid() || !reply.value()) {
        qWarning("Cannot ensure the shared microG worker is running: %s",
                 qPrintable(reply.isValid() ? QStringLiteral("invalid response") : reply.error().message()));
        return false;
    }
    return true;
}
}

int runMicrogWatchdog(int argc, char **argv)
{
    if (argc != 2) {
        qWarning("Invalid microG watchdog arguments.");
        return 2;
    }
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    QSettings settings;
    const QString provider = settings.value(QStringLiteral("googleServices/provider"),
                                            QStringLiteral("none")).toString();
    if (settings.status() != QSettings::NoError) {
        qWarning("Cannot read the Google services provider setting.");
        return 1;
    }
    if (provider != QLatin1String("microg") && provider != QLatin1String("none")) {
        qWarning("Invalid Google services provider setting.");
        return 1;
    }
    if (provider != QLatin1String("microg")) return 0;

    bool running = false;
    if (!managerRunning(&running)) return 1;
    if (running) return ensureWorker() ? 0 : 1;
    if (!QProcess::startDetached(ApplicationExecutables::manager(),
                                 QStringList() << QStringLiteral("--microg-service"))) {
        qWarning("Cannot start the background microG manager.");
        return 1;
    }

    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000) {
        QEventLoop wait;
        QTimer::singleShot(200, &wait, &QEventLoop::quit);
        wait.exec();
        if (!managerRunning(&running)) return 1;
        if (running) return ensureWorker() ? 0 : 1;
    }
    qWarning("The background microG manager did not register on D-Bus.");
    return 1;
}
