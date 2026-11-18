#include "../install_request_channel.h"
#include "../install_request_host.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
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

static bool MEGLER_CB picker(void *user, const megler_pick_request *)
{
    return *static_cast<bool *>(user);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray name("request_0123456789abcdef0123456789abcdef.apk");
    if (argc == 3) {
        bool pickerCalled = true;
        megler_system_host other = {};
        other.user = &pickerCalled;
        other.pick = picker;
        const QString endpoint = QString::fromLocal8Bit(argv[2]);
        InstallRequestHost owner(endpoint, QFileInfo(endpoint).dir().filePath(QStringLiteral("results")), other);
        check(owner.error().isEmpty(), "host initialized");
        const megler_system_host host = owner.host();
        check(host.pick && host.pick(host.user, nullptr), "picker callback delegated with original user");
        check(host.install_apk && !host.install_apk(host.user, "/data/a.apk", "../a.apk"), "invalid result name");
        QFile marker(owner.appsDir() + QStringLiteral("/apps.txt"));
        check(marker.open(QIODevice::ReadOnly) && marker.readAll() == "MEGLER_INSTALL_STATUS_V1\n",
              "capability published");
        QTimer poll;
        poll.setInterval(10);
        bool sent = false;
        QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
            if (!sent) {
                sent = host.install_apk(host.user, "/data/a.apk", name.constData());
                if (sent) check(!host.install_apk(host.user, "/data/b.apk", name.constData()), "single request");
                return;
            }
            QFile result(owner.appsDir() + QStringLiteral("/installs/") + QString::fromLatin1(name) + QStringLiteral(".txt"));
            if (!result.open(QIODevice::ReadOnly)) return;
            check(result.readAll() == "1\tinstalled\tactual commit", "result published after server response");
            app.quit();
        });
        QTimer::singleShot(5000, &app, [&]() { check(false, "host timed out"); });
        poll.start();
        return app.exec();
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    InstallRequestServer server(&process);
    QString error;
    check(server.listen(&error), "server listens");
    QObject::connect(&server, &InstallRequestServer::requested, &app,
                     [&](quint64 id, const QString &path, const QString &requestedName) {
        check(path == QLatin1String("/data/a.apk") && requestedName == QString::fromLatin1(name),
              "request forwarded");
        check(!server.finish(id, QStringLiteral("installed"), QStringLiteral("actual commit")),
              "success before confirmation rejected");
        check(server.approve(id), "approval from parent");
        check(server.finish(id, QStringLiteral("installed"), QStringLiteral("actual commit"),
                            QStringLiteral("org.example.app"), QStringLiteral("Example"), 42), "send result");
    });
    QObject::connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     &app, [&](int code, QProcess::ExitStatus status) {
        check(code == 0 && status == QProcess::NormalExit, "host child succeeds");
        app.quit();
    });
    process.start(QCoreApplication::applicationFilePath(),
                  QStringList() << QStringLiteral("--child") << server.endpoint());
    QTimer::singleShot(6000, &app, [&]() { check(false, "parent timed out"); });
    const int result = app.exec();
    std::puts("Install request host OK");
    return result;
}
