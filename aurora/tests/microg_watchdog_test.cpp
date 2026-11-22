#include "../microg_watchdog.h"
#include "../shared_microg_ipc.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>

namespace {
const char managerService[] = "ru.androidcompatibilitylayer.appmanager";

bool runWatchdog(bool expectedSuccess = true)
{
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(),
                  QStringList() << QStringLiteral("--microg-watchdog"));
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000) ||
        process.exitStatus() != QProcess::NormalExit ||
        (process.exitCode() == 0) != expectedSuccess) {
        std::fprintf(stderr, "Watchdog failed: %s\n", process.readAllStandardError().constData());
        return false;
    }
    return true;
}
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--microg-watchdog") == 0)
        return runMicrogWatchdog(argc, argv);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    if (argc > 1 && std::strcmp(argv[1], "--microg-service") == 0) {
        QFile marker(QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/launches"));
        if (!marker.open(QIODevice::Append) || marker.write("1\n") != 2) return 1;
        marker.close();
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.registerService(QString::fromLatin1(managerService))) return 1;
        SharedMicrogProviderObject provider(bus);
        bool running = false;
        provider.setWorkerCheckHandler([&running](QString *error) {
            if (QFile::exists(QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/fail-worker"))) {
                if (error) *error = QStringLiteral("Simulated worker failure.");
                return false;
            }
            if (running) return true;
            QFile starts(QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")) + QStringLiteral("/worker-starts"));
            if (!starts.open(QIODevice::Append) || starts.write("1\n") != 2) {
                if (error) *error = QStringLiteral("Cannot record the worker start.");
                return false;
            }
            running = true;
            return true;
        });
        QString error;
        if (!provider.start(&error)) return 1;
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("none"));
    settings.sync();
    const QString markerPath = QDir(directory.path()).filePath(QStringLiteral("launches"));
    if (settings.status() != QSettings::NoError || !runWatchdog() || QFile::exists(markerPath)) return 1;

    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();
    if (settings.status() != QSettings::NoError || !runWatchdog() || !runWatchdog()) return 1;
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != "1\n") return 1;
    marker.close();
    QFile starts(QDir(directory.path()).filePath(QStringLiteral("worker-starts")));
    if (!starts.open(QIODevice::ReadOnly) || starts.readAll() != "1\n") return 1;
    starts.close();
    QFile failure(QDir(directory.path()).filePath(QStringLiteral("fail-worker")));
    if (!failure.open(QIODevice::WriteOnly)) return 1;
    failure.close();
    if (!runWatchdog(false)) return 1;
    QEventLoop wait;
    QTimer::singleShot(3500, &wait, &QEventLoop::quit);
    wait.exec();
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    const QDBusReply<bool> registered = bus
        ? bus->isServiceRegistered(QString::fromLatin1(managerService)) : QDBusReply<bool>();
    return registered.isValid() && !registered.value() ? 0 : 1;
}
