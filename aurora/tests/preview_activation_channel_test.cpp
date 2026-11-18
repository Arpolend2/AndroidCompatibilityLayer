#include "../preview_activation_channel.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QWindow>
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char *description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 3) {
        const QByteArray mode(argv[1]);
        const QString endpoint = QString::fromLocal8Bit(argv[2]);
        if (mode == "--active" || mode == "--no-window" || mode == "--later" ||
            mode == "--hidden" || mode == "--delayed") {
            QWindow window;
            if (mode == "--active" || mode == "--delayed") {
                window.show();
                window.requestActivate();
            }
            PreviewActivationClient client(endpoint, mode == "--no-window" ? nullptr : &window);
            QObject::connect(&client, &PreviewActivationClient::channelError, &app,
                             [&](const QString &error) {
                std::fprintf(stderr, "Client error: %s\n", qPrintable(error));
                app.exit(1);
            });
            auto start = [&client]() {
                QString error;
                check(client.start(&error), "client starts");
            };
            if (mode == "--delayed")
                QTimer::singleShot(400, &client, start);
            else
                start();
            if (mode == "--later") {
                QTimer::singleShot(400, &window, [&window]() {
                    window.show();
                    window.requestActivate();
                });
            }
            QTimer::singleShot(5000, &app, &QCoreApplication::quit);
            return app.exec();
        }
        QLocalSocket socket;
        QByteArray input;
        if (mode == "--stale") {
            QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&]() {
                input += socket.readAll();
                while (input.contains('\n')) {
                    const int end = input.indexOf('\n');
                    const QList<QByteArray> parts = input.left(end).split(' ');
                    input.remove(0, end + 1);
                    check(parts.size() == 3, "child receives a framed request");
                    if (parts[1] == "2") {
                        socket.write("O 1\n");
                        QTimer::singleShot(60, &socket, [&socket]() { socket.write("O 2\n"); });
                    }
                }
            });
        }
        if (mode != "--idle") socket.connectToServer(endpoint);
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    auto runClient = [&](const char *mode, bool expected) {
        QProcess process;
        PreviewActivationServer server(&process);
        QString error;
        check(server.listen(&error), "server listens");
        QEventLoop loop;
        bool done = false;
        QObject::connect(&server, &PreviewActivationServer::completed, &loop,
                         [&](bool activated, const QString &message) {
            check(activated == expected && (activated ? message.isEmpty() : !message.isEmpty()),
                  "actual activation result returned");
            done = true;
            loop.quit();
        });
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QString::fromLatin1(mode) << server.endpoint());
        check(server.request(2000, &error), "request queued while process is starting");
        check(!server.request(2000, &error) && !error.isEmpty(), "concurrent request refused");
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        check(done, "client response received");
        process.kill();
        process.waitForFinished(2000);
    };
    runClient("--active", true);
    runClient("--no-window", false);
    runClient("--later", true);
    runClient("--hidden", false);

    {
        QProcess process;
        PreviewActivationServer server(&process);
        QString error;
        check(server.listen(&error), "delayed connection server listens");
        QEventLoop loop;
        int completed = 0;
        QObject::connect(&server, &PreviewActivationServer::completed, &loop,
                         [&](bool activated, const QString &) {
            ++completed;
            if (completed == 1) {
                check(!activated, "request timed out before child connected");
                check(server.request(1500, &error), "second request queued before child connected");
            } else {
                check(completed == 2 && activated, "only the new queued request was delivered");
                loop.quit();
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--delayed") << server.endpoint());
        check(server.request(100, &error), "request queued before delayed connection");
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        check(completed == 2, "delayed connection test finished");
        process.kill();
        process.waitForFinished(2000);
    }

    {
        QProcess process;
        PreviewActivationServer server(&process);
        QString error;
        check(server.listen(&error), "stale response server listens");
        QEventLoop loop;
        int completed = 0;
        QObject::connect(&server, &PreviewActivationServer::completed, &loop,
                         [&](bool activated, const QString &message) {
            ++completed;
            if (completed == 1) {
                check(!activated && !message.isEmpty(), "first request timed out");
                check(server.request(1000, &error), "next request accepted after timeout");
            } else {
                check(completed == 2 && activated && message.isEmpty(), "stale response ignored");
                loop.quit();
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--stale") << server.endpoint());
        check(server.request(200, &error), "first stale-test request queued");
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        check(completed == 2, "stale response test finished");
        process.kill();
        process.waitForFinished(2000);
    }
    {
        QProcess process;
        PreviewActivationServer server(&process);
        QString error;
        check(server.listen(&error), "authentication server listens");
        QLocalSocket impostor;
        QEventLoop loop;
        bool rejected = false, finished = false;
        QObject::connect(&process, &QProcess::started, &loop, [&]() {
            impostor.connectToServer(server.endpoint());
        });
        QObject::connect(&impostor, &QLocalSocket::disconnected, &loop, [&]() {
            rejected = true;
            process.kill();
        });
        QObject::connect(&server, &PreviewActivationServer::completed, &loop,
                         [&](bool activated, const QString &message) {
            check(!activated && !message.isEmpty(), "exit is reported as failure");
            finished = true;
            loop.quit();
        });
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--idle") << server.endpoint());
        check(server.request(2000, &error), "exit-test request queued");
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        check(rejected && finished, "foreign peer rejected and process exit reported");
        process.waitForFinished(2000);
        check(!server.request(1000, &error) && !error.isEmpty(), "exited process cannot be activated");
    }
    std::puts("Preview activation channel OK");
    return 0;
}
