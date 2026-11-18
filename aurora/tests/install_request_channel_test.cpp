#include "../install_request_channel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocalSocket>
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

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--idle") {
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (argc == 3) {
        InstallRequestClient client(QString::fromLocal8Bit(argv[2]));
        QTimer start;
        start.setInterval(10);
        QObject::connect(&start, &QTimer::timeout, &app, [&]() {
            if (!client.submit("/data/cache/base.apk", "app.apk")) return;
            start.stop();
            check(!client.submit("/data/cache/second.apk", "second.apk"), "concurrent client request rejected");
        });
        QObject::connect(&client, &InstallRequestClient::completed, &app,
                         [&](const QString &outcome, const QString &message, const QString &, const QString &, quint32) {
            check(outcome == QLatin1String("declined") && message == QLatin1String("no"),
                  "actual result delivered");
            check(client.submit("/data/cache/second.apk", "second.apk"), "next request accepted");
        });
        QTimer::singleShot(5000, &app, [&]() { check(false, "child timed out"); });
        start.start();
        return app.exec();
    }

    {
        QProcess target;
        InstallRequestServer server(&target);
        QString error;
        check(server.listen(&error), "authentication server listens");
        QLocalSocket impostor;
        QEventLoop loop;
        bool rejected = false;
        QObject::connect(&server, &InstallRequestServer::requested, &loop,
                         [&](quint64, const QString &, const QString &) { check(false, "foreign peer rejected"); });
        QObject::connect(&target, &QProcess::started, &loop, [&]() { impostor.connectToServer(server.endpoint()); });
        QObject::connect(&impostor, &QLocalSocket::connected, &loop, [&]() {
            impostor.write("{\"path\":\"/data/app.apk\",\"name\":\"app.apk\"}\n");
        });
        QObject::connect(&impostor, &QLocalSocket::disconnected, &loop, [&]() {
            rejected = true;
            loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        target.start(QCoreApplication::applicationFilePath(),
                     QStringList() << QStringLiteral("--idle") << server.endpoint());
        loop.exec();
        target.kill();
        target.waitForFinished(2000);
        check(rejected, "foreign peer disconnected");
    }

    QProcess process;
    InstallRequestServer server(&process);
    QString error;
    check(server.listen(&error), "server listens");
    quint64 first = 0, second = 0;
    bool canceled = false;
    int count = 0;
    QObject::connect(&server, &InstallRequestServer::requested, &app,
                     [&](quint64 id, const QString &path, const QString &name) {
        ++count;
        if (count == 1) {
            first = id;
            check(path == QLatin1String("/data/cache/base.apk") && name == QLatin1String("app.apk"),
                  "request delivered");
            check(!server.finish(id + 1, QStringLiteral("installed"), QString()), "stale id rejected");
            check(!server.finish(id, QStringLiteral("unknown"), QString()), "invalid outcome rejected");
            check(server.finish(id, QStringLiteral("declined"), QStringLiteral("no")), "complete first request");
            check(!server.finish(id, QStringLiteral("installed"), QString()), "duplicate result rejected");
        } else {
            check(count == 2 && path == QLatin1String("/data/cache/second.apk"), "second request delivered");
            second = id;
            check(second != first, "request IDs differ");
            process.kill();
        }
    });
    QObject::connect(&server, &InstallRequestServer::canceled, &app, [&](quint64 id) {
        check(id == second && id != 0, "sender exit cancels active request");
        canceled = true;
    });
    QObject::connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     &app, [&](int, QProcess::ExitStatus) {
        check(canceled && count == 2, "cancellation before process finished");
        check(!server.finish(second, QStringLiteral("installed"), QString()), "result after exit rejected");
        app.quit();
    });
    process.start(QCoreApplication::applicationFilePath(),
                  QStringList() << QStringLiteral("--child") << server.endpoint());
    QTimer::singleShot(5000, &app, [&]() { check(false, "parent timed out"); });
    const int result = app.exec();
    check(result == 0 && canceled, "channel test finished");
    std::puts("Install request channel OK");
    return 0;
}
