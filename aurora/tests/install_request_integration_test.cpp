#include "../install_request_coordinator.h"
#include "../install_request_host.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char *description)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray name("request_0123456789abcdef0123456789abcdef.apk");
    if (argc == 5 && QByteArray(argv[1]) == "--child") {
        const QString mode = QString::fromLocal8Bit(argv[4]);
        megler_system_host other = {};
        const QString endpoint = QString::fromLocal8Bit(argv[2]);
        InstallRequestHost owner(endpoint, QFileInfo(endpoint).dir().filePath(QStringLiteral("results")), other);
        const megler_system_host host = owner.host();
        QTimer poll;
        bool submitted = false;
        QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
            if (!submitted) {
                submitted = host.install_apk(host.user, argv[3], name.constData());
                return;
            }
            QFile result(owner.appsDir() + QStringLiteral("/installs/") + QString::fromLatin1(name) + QStringLiteral(".txt"));
            if (!result.open(QIODevice::ReadOnly)) return;
            const QByteArray status = result.readAll();
            check(status.contains(mode == QLatin1String("accept") ? "\tinstalled\t" : "\tdeclined\t"), "correct status");
            if (mode == QLatin1String("accept")) {
                QFile apps(owner.appsDir() + QStringLiteral("/apps.txt"));
                check(apps.open(QIODevice::ReadOnly) && apps.readAll().contains("megler.test.hello\t"),
                      "installed app visible before success");
            }
            app.quit();
        });
        poll.start(10);
        QTimer::singleShot(10000, &app, [&]() { check(false, "client timed out"); });
        return app.exec();
    }
    if (argc != 2) return 2;
    QTemporaryDir directory;
    check(directory.isValid(), "temporary root");
    const QString data = directory.path() + QStringLiteral("/data");
    const QString session = data + QStringLiteral("/cache/megler-session-1");
    check(QDir().mkpath(session), "session directory");
    const QString apk = session + QStringLiteral("/UNIVERSAL_APK.apk");
    check(QFile::copy(QString::fromLocal8Bit(argv[1]), apk), "copy fixture");

    for (const QString &mode : {QStringLiteral("decline"), QStringLiteral("exit"), QStringLiteral("accept")}) {
        QProcess process;
        process.setProcessChannelMode(QProcess::ForwardedChannels);
        const QString root = directory.path() + QLatin1Char('/') + mode;
        InstallRequestCoordinator coordinator(&process, data, root);
        QString error;
        check(coordinator.listen(&error), "listen");
        bool installed = false;
        QObject::connect(&coordinator, &InstallRequestCoordinator::installed, &app, [&]() { installed = true; });
        QObject::connect(&coordinator, &InstallRequestCoordinator::confirmationRequested, &app,
                         [&](quint64 id, const QString &, const QString &package, quint32) {
            check(package == QLatin1String("megler.test.hello"), "inspected package shown");
            if (mode == QLatin1String("exit")) process.kill();
            else coordinator.decision(id, mode == QLatin1String("accept"));
        });
        QEventLoop loop;
        QObject::connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                         &loop, [&](int code, QProcess::ExitStatus status) {
            check(mode == QLatin1String("exit") ? status == QProcess::CrashExit
                                                : status == QProcess::NormalExit && code == 0,
                  "client exits as expected");
            loop.quit();
        });
        QTimer::singleShot(12000, &loop, &QEventLoop::quit);
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--child") << coordinator.endpoint() << apk << mode);
        loop.exec();
        check(process.state() == QProcess::NotRunning, "process finished");
        const bool present = QFile::exists(root + QStringLiteral("/megler.test.hello/base.apk"));
        check(present == (mode == QLatin1String("accept")) && present == installed,
              "only confirmed request committed");
    }
    std::puts("Install request integration OK");
    return 0;
}
