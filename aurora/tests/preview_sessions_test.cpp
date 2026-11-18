#include "../preview_sessions.h"
#include "../install_request_host.h"
#include "../log_channel.h"
#include "../diagnostic_log.h"
#include "../preview_activation_channel.h"
#include "../../core/src/apk/mg_apk.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QLocalSocket>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

static void check(bool ok, const char *description)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    std::memset(info, 0, sizeof(*info));
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray contents = file.readAll();
    if (contents != "first" && contents != "second" && contents != "cancel") return false;
    std::strcpy(info->package, contents == "first" ? "org.example.first" :
                              contents == "second" ? "org.example.second" : "org.example.cancel");
    std::strcpy(info->label, "Example");
    info->runnable = true;
    return true;
}

static void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "open input");
    check(file.write(contents) == contents.size(), "write input");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray request("request_0123456789abcdef0123456789abcdef.apk");
    if (argc == 8 && QByteArray(argv[1]) == "--preview") {
        const QString data = QString::fromLocal8Bit(argv[3]);
        const QByteArray tag = QFile::encodeName(QFileInfo(data).fileName());
        std::fprintf(stderr, "preview output %s\n", tag.constData());
        const QByteArray logPath = qgetenv("MEGLER_CORE_LOG_SOCKET");
        if (!logPath.isEmpty()) {
            int socket = LogChannel::openSender(QString::fromLocal8Bit(logPath));
            check(socket >= 0, "child log socket");
            LogChannel::sendLine(0, tag.constData(), &socket);
            ::close(socket);
        }
        megler_system_host other = {};
        InstallRequestHost host(QString::fromLocal8Bit(argv[5]), QString::fromLocal8Bit(argv[6]), other);
        check(host.error().isEmpty(), "child install host");
        QScopedPointer<PreviewActivationClient> activation;
        QScopedPointer<QLocalSocket> disconnectedActivation;
        if (tag == "cancel") {
            disconnectedActivation.reset(new QLocalSocket);
            QObject::connect(disconnectedActivation.data(), &QLocalSocket::connected, &app, [&]() {
                QTimer::singleShot(100, disconnectedActivation.data(), &QLocalSocket::disconnectFromServer);
            });
            disconnectedActivation->connectToServer(QString::fromLocal8Bit(argv[7]));
        } else {
            activation.reset(new PreviewActivationClient(QString::fromLocal8Bit(argv[7]), nullptr));
            QString activationError;
            check(activation->start(&activationError), "child activation channel starts");
        }
        const megler_system_host callbacks = host.host();
        bool submitted = false;
        bool received = false;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
            if (!submitted) {
                const QByteArray path = QFile::encodeName(data + QStringLiteral("/source.apk"));
                submitted = callbacks.install_apk(callbacks.user, path.constData(), request.constData());
                if (submitted && tag == "cancel")
                    QTimer::singleShot(1000, &app, &QCoreApplication::quit);
                return;
            }
            if (tag == "cancel") return;
            if (received) return;
            QFile result(host.appsDir() + QStringLiteral("/installs/") + QString::fromLatin1(request) + QStringLiteral(".txt"));
            if (!result.open(QIODevice::ReadOnly)) return;
            const QByteArray expected = tag == "first" ? "\tinstalled\t" : "\tdeclined\t";
            const QByteArray contents = result.readAll();
            if (!contents.contains(expected))
                std::fprintf(stderr, "%s received: %s\n", tag.constData(), contents.constData());
            check(contents.contains(expected), "response matched its originating process");
            received = true;
            QTimer::singleShot(tag == "first" ? 0 : 500, &app, &QCoreApplication::quit);
        });
        poll.start(10);
        QTimer::singleShot(10000, &app, [&]() { check(false, "child timeout"); });
        return app.exec();
    }

    QTemporaryDir directory;
    check(directory.isValid(), "temporary root");
    const QString first = directory.path() + QStringLiteral("/first");
    const QString second = directory.path() + QStringLiteral("/second");
    const QString cancel = directory.path() + QStringLiteral("/cancel");
    const QString store = directory.path() + QStringLiteral("/store");
    check(QDir().mkpath(first) && QDir().mkpath(second) && QDir().mkpath(cancel), "create data roots");
    writeFile(first + QStringLiteral("/source.apk"), "first");
    writeFile(second + QStringLiteral("/source.apk"), "second");
    writeFile(cancel + QStringLiteral("/source.apk"), "cancel");
    PreviewSessions sessions;
    QMap<QString, QString> requests;
    QMap<QString, QString> logs;
    int ended = 0;
    bool installed = false;
    bool canceled = false;
    bool decisionsSent = false;
    QMap<QString, QString> activationInstances;
    QEventLoop loop;
    auto decideWhenReady = [&]() {
        if (decisionsSent || requests.size() != 3 || activationInstances.size() != 3) return;
        decisionsSent = true;
        sessions.decision(requests.value(QStringLiteral("org.example.first")), true);
        sessions.decision(requests.value(QStringLiteral("org.example.first")), false);
        sessions.decision(requests.value(QStringLiteral("org.example.second")), false);
    };
    QObject::connect(&sessions, &PreviewSessions::installed, &app, [&]() { installed = true; });
    QObject::connect(&sessions, &PreviewSessions::activationFinished, &app,
                     [&](const QString &instance, const QString &key, bool activated, const QString &error) {
        check(!instance.isEmpty() && !activated && !error.isEmpty(), "activation refusal is explicit");
        check((key == QLatin1String("first") || key == QLatin1String("second") ||
               key == QLatin1String("cancel")) &&
              !activationInstances.contains(key), "activation result identifies the correct session");
        if (key == QLatin1String("cancel"))
            check(error.contains(QLatin1String("disconnected")), "activation channel disconnect is reported");
        activationInstances.insert(key, instance);
        check(sessions.running(QStringLiteral("first")) && sessions.running(QStringLiteral("second")) &&
              sessions.running(QStringLiteral("cancel")), "activation failure does not stop any process");
        decideWhenReady();
    });
    QObject::connect(&sessions, &PreviewSessions::logChanged, &app,
                     [&](const QString &, const QString &key, const QString &text) { logs[key] = text; });
    QObject::connect(&sessions, &PreviewSessions::confirmationRequested, &app,
                     [&](const QString &token, const QString &, const QString &package, quint32) {
        check(!requests.contains(package), "one request per process");
        requests[package] = token;
        if (requests.size() != 3) return;
        check(requests.value(QStringLiteral("org.example.first")) != requests.value(QStringLiteral("org.example.second")),
              "globally distinct request tokens");
        check(requests.value(QStringLiteral("org.example.cancel")) != requests.value(QStringLiteral("org.example.second")) &&
              requests.value(QStringLiteral("org.example.cancel")) != requests.value(QStringLiteral("org.example.first")),
              "canceled request has its own token");
        decideWhenReady();
    });
    QObject::connect(&sessions, &PreviewSessions::confirmationCanceled, &app, [&](const QString &token) {
        check(token == requests.value(QStringLiteral("org.example.cancel")), "only canceled request withdrawn");
        canceled = true;
    });
    QObject::connect(&sessions, &PreviewSessions::finished, &app,
                     [&](const QString &, const QString &key, int code, bool crashed, const QString &error) {
        check(!crashed && code == 0 && error.isEmpty(), "child exits successfully");
        if (key == QLatin1String("first"))
            check(sessions.running(QStringLiteral("second")), "other child survives first exit");
        if (++ended == 3) QTimer::singleShot(300, &loop, &QEventLoop::quit);
    });
    QString error;
    check(sessions.start(QStringLiteral("first"), directory.path(), first, QStringLiteral("apk"), store, &error),
          "start first");
    check(sessions.start(QStringLiteral("second"), directory.path(), second, QStringLiteral("apk"), store, &error),
          "start second");
    check(sessions.start(QStringLiteral("cancel"), directory.path(), cancel, QStringLiteral("apk"), store, &error),
          "start third");
    check(!sessions.start(QStringLiteral("first"), directory.path(), first, QStringLiteral("apk"), store, &error)
          && !error.isEmpty(), "duplicate rejected");
    check(sessions.count() == 3, "all children running");
    const QByteArray oldTmp = qgetenv("TMPDIR");
    const bool hadTmp = qEnvironmentVariableIsSet("TMPDIR");
    const QString longTmp = directory.path() + QLatin1Char('/') + QString(100, QLatin1Char('x'));
    check(QDir().mkpath(longTmp), "create long temporary directory");
    qputenv("TMPDIR", QFile::encodeName(longTmp));
    const bool startedWithBadChannel =
        sessions.start(QStringLiteral("broken"), directory.path(), first, QStringLiteral("apk"), store, &error);
    if (hadTmp) qputenv("TMPDIR", oldTmp);
    else qunsetenv("TMPDIR");
    check(!startedWithBadChannel && !error.isEmpty() && sessions.count() == 3 &&
          sessions.running(QStringLiteral("first")) && sessions.running(QStringLiteral("second")) &&
          sessions.running(QStringLiteral("cancel")) && !sessions.running(QStringLiteral("broken")),
          "IPC startup failure leaves other sessions unchanged");
    struct rlimit originalLimit;
    check(getrlimit(RLIMIT_NOFILE, &originalLimit) == 0, "read file descriptor limit");
    int maxFd = 2;
    const QStringList descriptors = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString &descriptor : descriptors) {
        bool ok = false;
        const int fd = descriptor.toInt(&ok);
        if (ok && fd > maxFd) maxFd = fd;
    }
    struct rlimit limited = originalLimit;
    limited.rlim_cur = qMin<rlim_t>(originalLimit.rlim_cur, rlim_t(maxFd + 16));
    check(setrlimit(RLIMIT_NOFILE, &limited) == 0, "limit descriptors for activation failure test");
    QList<int> heldDescriptors;
    int fd;
    while ((fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0) heldDescriptors.append(fd);
    check(errno == EMFILE && heldDescriptors.size() >= 2, "exhaust spare descriptors");
    ::close(heldDescriptors.takeLast());
    ::close(heldDescriptors.takeLast());
    const bool startedWithoutActivation =
        sessions.start(QStringLiteral("activation-broken"), directory.path(), first, QStringLiteral("apk"), store, &error);
    const QString channelError = error;
    check(setrlimit(RLIMIT_NOFILE, &originalLimit) == 0, "restore descriptor limit");
    for (int held : heldDescriptors) ::close(held);
    check(!startedWithoutActivation && channelError.contains(QLatin1String("preview activation channel")) &&
          sessions.count() == 3 && sessions.running(QStringLiteral("first")) &&
          sessions.running(QStringLiteral("second")) && sessions.running(QStringLiteral("cancel")) &&
          !sessions.running(QStringLiteral("activation-broken")),
          "activation channel startup failure leaves other sessions unchanged");
    check(!sessions.activate(QStringLiteral("missing"), 2000, &error) && !error.isEmpty(),
          "missing session cannot be activated");
    check(sessions.activate(QStringLiteral("first"), 2000, &error), "activate first while starting");
    check(!sessions.activate(QStringLiteral("first"), 2000, &error) && !error.isEmpty(),
          "duplicate activation is rejected");
    check(sessions.activate(QStringLiteral("second"), 2000, &error), "activate second independently");
    check(sessions.activate(QStringLiteral("cancel"), 2000, &error), "activate session with failing channel");
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    loop.exec();
    check(ended == 3 && sessions.count() == 0 && canceled && decisionsSent &&
          activationInstances.size() == 3 &&
          activationInstances.value(QStringLiteral("first")) != activationInstances.value(QStringLiteral("second")),
          "all children finished and independent activations were reported");
    check(installed && QFile::exists(store + QStringLiteral("/org.example.first/base.apk")) &&
          !QFile::exists(store + QStringLiteral("/org.example.second/base.apk")), "only approved app installed");
    check(logs.value(QStringLiteral("first")).contains(QStringLiteral("first")) &&
          !logs.value(QStringLiteral("first")).contains(QStringLiteral("second")) &&
          logs.value(QStringLiteral("second")).contains(QStringLiteral("second")), "separate logs");
    {
        DiagnosticLog log;
        check(log.start(&error), "start diagnostic capture");
        log.startNotifications();
        PreviewSessions killed;
        killed.setDiagnosticLog(&log);
        const QString killStore = directory.path() + QStringLiteral("/kill-store");
        QEventLoop killLoop;
        int killedCount = 0;
        QObject::connect(&killed, &PreviewSessions::finished, &app,
                         [&](const QString &, const QString &, int, bool crashed, const QString &) {
            check(crashed, "explicitly killed child reports crash");
            if (++killedCount == 2) killLoop.quit();
        });
        check(!killed.kill(QStringLiteral("missing")), "missing process cannot be killed");
        check(killed.start(QStringLiteral("kill-early"), directory.path(), cancel, QStringLiteral("apk"), killStore, &error),
              "start process to kill immediately");
        check(killed.start(QStringLiteral("kill-later"), directory.path(), first, QStringLiteral("apk"), killStore, &error),
              "start separate process");
        check(killed.kill(QStringLiteral("kill-early")), "kill starting process");
        check(killed.running(QStringLiteral("kill-later")), "other process still running");
        QTimer::singleShot(300, &app, [&]() {
            check(log.text().contains(QStringLiteral("preview output first")), "capture preview stderr");
            check(killed.kill(QStringLiteral("kill-later")), "kill running process");
        });
        QTimer::singleShot(5000, &killLoop, &QEventLoop::quit);
        killLoop.exec();
        check(killedCount == 2 && killed.count() == 0, "killed processes finished independently");
        check(!killed.kill(QStringLiteral("kill-later")), "finished process cannot be killed");
    }
    std::puts("Preview sessions OK");
    return 0;
}
