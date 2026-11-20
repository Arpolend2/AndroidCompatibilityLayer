#include "../manager_lifecycle.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {
class IdentityFixture : public QDBusVirtualObject
{
public:
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("ru.omp.am.Identify") ||
            message.member() != QLatin1String("IdentifyByBusName")) return false;
        if (message.arguments().size() != 1 || message.arguments().first().type() != QVariant::String) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad arguments")));
            return true;
        }
        sender = message.arguments().first().toString();
        connection.send(message.createReply(QVariantList() << QStringLiteral("/test/client")
                                                           << QStringLiteral("test-key")));
        return true;
    }
    QString sender;
};

void check(bool value, const char *description)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

void append(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Append), "open test event log");
    check(file.write(data) == data.size(), "write test event log");
}

QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    return file.readAll();
}

QString diagnosticFile;
void captureDiagnostics(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(QLatin1String("APK handoff:")))
        append(diagnosticFile, message.toUtf8() + '\n');
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
        timer.start(5000);
        loop.exec();
    }
    check(watcher.isFinished(), "D-Bus call completed");
    return watcher;
}

int server(QCoreApplication &app, const QString &eventFile, bool failGui, bool bounded, bool keepAlive)
{
    diagnosticFile = eventFile + QStringLiteral(".diagnostics");
    qInstallMessageHandler(captureDiagnostics);
    bool visible = false;
    ManagerLifecycle lifecycle([&]() {
        if (failGui) return false;
        if (!visible) append(eventFile, "GUI\n");
        visible = true;
        return true;
    }, nullptr, 1500);
    lifecycle.setKeepAlive(keepAlive);
    check(lifecycle.recordStoreResult(QStringLiteral("/test/claimed_0123456789abcdef0123456789abcdef.apk"),
                                      true, QByteArray(64, 'a')),
          "record imported snapshot result");
    check(!lifecycle.recordStoreResult(QStringLiteral("/test/claimed_1123456789abcdef0123456789abcdef.apk"),
                                       true, QByteArray("invalid")),
          "invalid imported snapshot fingerprint is rejected");
    lifecycle.setLaunchHandler([&eventFile, &app](const QString &package,
                                                  const std::function<void(const QString &)> &reply) {
        append(eventFile, "LAUNCH " + package.toUtf8() + '\n');
        QTimer::singleShot(50, &app, [reply, package]() {
            reply(package == QLatin1String("org.example.app") ? QStringLiteral("ok")
                                                              : QStringLiteral("error:not available"));
        });
    });
    QObject::connect(&lifecycle, &ManagerLifecycle::fileRequested, [&eventFile](const QString &path) {
        append(eventFile, "FILE " + path.toUtf8() + '\n');
    });
    QObject::connect(&lifecycle, &ManagerLifecycle::fileRejected, [&eventFile](const QString &) {
        append(eventFile, "REJECTED\n");
    });
    QString error;
    if (!lifecycle.start(true, &error)) {
        std::fprintf(stderr, "Cannot start: %s\n", qPrintable(error));
        return 1;
    }
    std::puts("READY");
    std::fflush(stdout);
    if (bounded) QTimer::singleShot(6000, &app, &QCoreApplication::quit);
    return app.exec();
}

void startServer(QProcess &process, const QString &eventFile, bool failGui = false)
{
    process.start(QCoreApplication::applicationFilePath(),
                  QStringList() << QStringLiteral("--server") << eventFile
                                << (failGui ? QStringLiteral("fail") : QStringLiteral("ok")));
    check(process.waitForStarted(5000), "start manager process");
    check(process.waitForReadyRead(5000), "manager process becomes ready");
    check(process.readAllStandardOutput().contains("READY"), "manager registered service");
}

void waitEvents(const QString &path, const QByteArray &expected)
{
    for (int i = 0; i < 100 && !read(path).contains(expected); ++i) QThread::msleep(20);
    check(read(path).contains(expected), "manager delivered request");
}

void waitEventCount(const QString &path, const QByteArray &event, int count)
{
    for (int i = 0; i < 100 && read(path).count(event) < count; ++i) QThread::msleep(20);
    check(read(path).count(event) == count, "manager delivered expected event count");
}

void sendOpenFile(const QDBusConnection &bus, const QString &value)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("openFile"));
    message << (QStringList() << value);
    check(bus.send(message), "send openFile");
}

bool waitForOwner(const QDBusConnection &bus, bool expected)
{
    for (int i = 0; i < 250; ++i) {
        const QDBusReply<bool> registered =
            bus.interface()->isServiceRegistered(QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        if (registered.isValid() && registered.value() == expected) return true;
        QThread::msleep(20);
    }
    return false;
}

int activationClient(const QString &eventFile)
{
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();
    check(settings.status() == QSettings::NoError, "save activation test settings");
    const QDBusConnection bus = QDBusConnection::sessionBus();
    check(waitForOwner(bus, false), "activation starts without a manager");
    QString provider, error;
    check(ManagerLifecycle::forwardToRunningManager(QStringList(), &error) == ManagerLifecycle::NoOwner,
          "normal launch does not auto-activate even with an installed service");
    check(waitForOwner(bus, false), "normal launch left service inactive");
    QDBusPendingReply<QString> reply(waitFor(requestSharedMicrogProvider(bus)));
    check(readSharedMicrogProvider(reply, &provider, &error) && provider == QLatin1String("microg"),
          "D-Bus auto-activates the background provider");
    check(read(eventFile).isEmpty(), "auto-activation does not open a window");
    const QDBusReply<uint> firstPid = bus.interface()->servicePid(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
    check(firstPid.isValid(), "read activated manager process ID");
    check(waitForOwner(bus, false), "auto-activated manager exits after idle");
    reply = QDBusPendingReply<QString>(waitFor(requestSharedMicrogProvider(bus)));
    check(readSharedMicrogProvider(reply, &provider, &error), "provider auto-activates again");
    const QDBusReply<uint> secondPid = bus.interface()->servicePid(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
    check(secondPid.isValid() && secondPid.value() != firstPid.value(), "new manager process after idle");
    const QStringList placeholder{QStringLiteral("%u")};
    check(ManagerLifecycle::forwardToRunningManager(ManagerLifecycle::launchFileArguments(placeholder), &error) ==
          ManagerLifecycle::Delivered, "debug launch with an unexpanded field code opens the auto-activated manager");
    waitEvents(eventFile, "GUI\n");
    QThread::msleep(2000);
    check(waitForOwner(bus, true), "manager remains available after opening the window");
    check(waitForOwner(bus, false), "bounded test manager exits");
    std::puts("Manager activation tests passed");
    return 0;
}

int actualManager(const QString &binary)
{
    QTemporaryDir directory;
    check(directory.isValid(), "create isolated actual manager configuration");
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("minimal"));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), directory.path());
    environment.insert(QStringLiteral("XDG_DATA_HOME"), directory.path());
    process.setProcessEnvironment(environment);
    process.start(binary, QStringList() << QStringLiteral("--microg-service"));
    check(process.waitForStarted(5000), "start actual manager");
    QDBusConnection bus = QDBusConnection::sessionBus();
    IdentityFixture identity;
    check(bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity),
          "register test identity object for actual manager");
    check(bus.registerService(QStringLiteral("ru.omp.am")), "register test identity service for actual manager");
    if (!waitForOwner(bus, true)) {
        process.waitForFinished(100);
        std::fprintf(stderr, "Manager exit=%d status=%d, stdout=%s, stderr=%s\n",
                     process.exitCode(), int(process.exitStatus()), process.readAllStandardOutput().constData(),
                     process.readAllStandardError().constData());
        process.terminate();
        process.waitForFinished(5000);
        check(false, "actual manager registers its service");
    }
    QDBusPendingReply<QString> reply(waitFor(requestSharedMicrogProvider(bus)));
    QString provider, error;
    check(readSharedMicrogProvider(reply, &provider, &error), "actual manager serves provider request");
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("InspectCaller"));
    QDBusPendingReply<QStringList> identified(waitFor(bus.asyncCall(request)));
    check(!identified.isError() &&
          identified.value() == (QStringList() << bus.baseService()
                                 << QStringLiteral("/test/client") << QStringLiteral("test-key")) &&
          identity.sender == bus.baseService(), "actual manager identifies the requesting bus connection");
    check(process.state() == QProcess::Running, "actual manager stays running without a window");
    process.terminate();
    check(process.waitForFinished(5000), "stop actual test manager");
    bus.unregisterService(QStringLiteral("ru.omp.am"));
    bus.unregisterObject(QStringLiteral("/ru/omp/am"));
    std::puts("Actual manager background startup test passed");
    return 0;
}
}

int main(int argc, char **argv)
{
    qputenv("AURORA_APP_ID", "ru.androidcompatibilitylayer.appmanager");
    qputenv("AURORA_APP_INSTANCE_ID", "ru.androidcompatibilitylayer.appmanager");
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    if (argc == 4 && QByteArray(argv[1]) == "--server")
        return server(app, QString::fromLocal8Bit(argv[2]), QByteArray(argv[3]) == "fail",
                      QByteArray(argv[3]) == "auto", QByteArray(argv[3]) == "keep");
    if (argc == 3 && QByteArray(argv[1]) == "--activation-client")
        return activationClient(QString::fromLocal8Bit(argv[2]));
    if (argc == 3 && QByteArray(argv[1]) == "--actual-manager")
        return actualManager(QString::fromLocal8Bit(argv[2]));
    if (argc == 3 && QByteArray(argv[1]) == "--desktop") {
        QFile desktop(QString::fromLocal8Bit(argv[2]));
        check(desktop.open(QIODevice::ReadOnly), "open manager desktop");
        bool applicationSection = false;
        bool allowsMultipleInstances = false;
        QByteArray exported, exec;
        while (!desktop.atEnd()) {
            const QByteArray line = desktop.readLine().trimmed();
            if (line.startsWith('[')) applicationSection = line == "[X-Application]";
            else if (applicationSection && line.startsWith("ExportDBusInterfaces="))
                exported = line.mid(int(sizeof("ExportDBusInterfaces=") - 1));
            else if (applicationSection && line.startsWith("ExecDBus="))
                exec = line.mid(int(sizeof("ExecDBus=") - 1));
            else if (!applicationSection && line == "X-Nemo-Single-Instance=no")
                allowsMultipleInstances = true;
        }
        const QList<QByteArray> exports = exported.split(';');
        check(exports.contains(QByteArray("ru.androidcompatibilitylayer.appmanager.log")),
              "manager desktop exports log D-Bus interface");
        check(exec == QByteArray("/usr/bin/ru.androidcompatibilitylayer.appmanager --microg-service"),
              "manager desktop activates background manager");
        check(!allowsMultipleInstances, "manager desktop does not allow multiple instances");
        std::puts("Manager desktop contract OK");
        return 0;
    }

    QTemporaryDir directory;
    check(directory.isValid(), "create test directory");
    const QStringList uri{QStringLiteral("file:///tmp/sample.apk")};
    const QStringList malformed{QStringLiteral("file:///tmp/sample%u.apk")};
    const QStringList extra{QStringLiteral("%u"), uri.first()};
    check(ManagerLifecycle::launchFileArguments(uri) == uri &&
          ManagerLifecycle::launchFileArguments(malformed) == malformed &&
          ManagerLifecycle::launchFileArguments(extra) == extra,
          "startup only ignores the sole unexpanded desktop field code");
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();
    check(settings.status() == QSettings::NoError, "save test settings");
    const QString eventFile = directory.path() + QStringLiteral("/events");
    QDBusConnection bus = QDBusConnection::sessionBus();
    check(bus.isConnected(), "connect to D-Bus");
    QString error;
    check(ManagerLifecycle::forwardToRunningManager(QStringList(), &error) == ManagerLifecycle::NoOwner,
          "normal launch does not auto-activate an absent manager");

    {
        QProcess process;
        process.start(app.applicationFilePath(), QStringList() << QStringLiteral("--server") << eventFile
                                                               << QStringLiteral("keep"));
        check(process.waitForStarted(5000) && process.waitForReadyRead(5000) &&
              process.readAllStandardOutput().contains("READY"), "start retained background manager");
        QThread::msleep(2000);
        check(process.state() == QProcess::Running, "retained background manager survives idle timeout");
        process.terminate();
        check(process.waitForFinished(5000), "stop retained background manager");
    }
    {
        QProcess process;
        startServer(process, eventFile);
        QDBusPendingReply<QString> reply(waitFor(requestSharedMicrogProvider(bus)));
        QString provider;
        check(readSharedMicrogProvider(reply, &provider, &error) && provider == QLatin1String("microg"),
              "background provider reads manager settings");
        check(read(eventFile).isEmpty(), "provider does not open a window");
        QThread::msleep(1000);
        reply = QDBusPendingReply<QString>(waitFor(requestSharedMicrogProvider(bus)));
        check(readSharedMicrogProvider(reply, &provider, &error), "refresh background idle timer");
        QThread::msleep(750);
        check(process.state() == QProcess::Running, "request extends background idle timeout");
        check(process.waitForFinished(3000) && process.exitCode() == 0,
              "idle background manager exits");
        check(read(eventFile).isEmpty(), "idle background manager never opens a window");
    }
    {
        QProcess process;
        startServer(process, eventFile);
        ManagerLifecycle competing([]() { return true; }, nullptr, 1500);
        check(!competing.start(true, &error) && !error.isEmpty(), "second owner is rejected");
        QDBusMessage status = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("storeResult"));
        status << QStringLiteral("1123456789abcdef0123456789abcdef");
        QDBusMessage pending = QDBusPendingCallWatcher(waitFor(bus.asyncCall(status))).reply();
        check(pending.type() == QDBusMessage::ReplyMessage && pending.arguments().value(0).toString().isEmpty(),
              "unknown store result remains pending");
        status.setArguments(QVariantList());
        status << QStringLiteral("0123456789abcdef0123456789abcdef");
        const QDBusMessage statusReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(status))).reply();
        check(statusReply.type() == QDBusMessage::ReplyMessage &&
              statusReply.arguments().value(0).toString() == QStringLiteral("installed:") + QString(64, QLatin1Char('a')),
              "only the recorded import fingerprint is returned");
        waitFor(bus.asyncCall(status));
        QDBusMessage library = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("libraryApps"));
        check(QDBusPendingCallWatcher(waitFor(bus.asyncCall(library))).reply().type() == QDBusMessage::ReplyMessage,
              "library query is answered");
        waitFor(bus.asyncCall(library));
        const QByteArray diagnostics = read(eventFile + QStringLiteral(".diagnostics"));
        if (diagnostics.count("manager received a store result query") != 1 ||
            diagnostics.count("manager returned a recorded store result") != 1 ||
            diagnostics.count("manager received a library list query") != 1)
            std::fprintf(stderr, "D-Bus diagnostics: %s\n", diagnostics.constData());
        check(diagnostics.count("manager received a store result query") == 1 &&
              diagnostics.count("manager returned a recorded store result") == 1 &&
              diagnostics.count("manager received a library list query") == 1 &&
              !diagnostics.contains("0123456789abcdef"), "store diagnostics are bounded and omit request IDs");
        QDBusMessage launch = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("launchApp"));
        launch << QStringLiteral("org.example.app");
        QDBusMessage launchReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(launch))).reply();
        check(launchReply.type() == QDBusMessage::ReplyMessage &&
              launchReply.arguments().value(0).toString() == QLatin1String("ok"),
              "delayed launch result reaches caller");
        waitEvents(eventFile, "LAUNCH org.example.app\n");
        launch.setArguments(QVariantList() << QStringLiteral("../invalid"));
        launchReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(launch))).reply();
        check(launchReply.arguments().value(0).toString().startsWith(QLatin1String("error:")),
              "invalid launch package is rejected");
        launch.setArguments(QVariantList() << QStringLiteral("org.example.missing"));
        launchReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(launch))).reply();
        check(launchReply.arguments().value(0).toString() == QLatin1String("error:not available"),
              "launch failure reaches caller");
        QDBusMessage activation = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("activate"));
        activation << (QStringList() << QStringLiteral("file:///home/user/%GG.apk"));
        const QDBusMessage invalidActivation = bus.call(activation, QDBus::Block, 5000);
        check(invalidActivation.type() == QDBusMessage::ReplyMessage &&
              !invalidActivation.arguments().value(0).toBool(), "malformed activation is rejected");
        waitEventCount(eventFile, "REJECTED\n", 1);
        check(!read(eventFile).contains("GUI\n"), "malformed activation does not open manager window");
        check(ManagerLifecycle::forwardToRunningManager(
                  QStringList() << QStringLiteral("file:///home/user/forwarded%20app.apk"),
                  &error) == ManagerLifecycle::Delivered, "normal launch activates running service");
        waitEvents(eventFile, "FILE /home/user/forwarded app.apk\n");
        check(read(eventFile).contains("GUI\nFILE /home/user/forwarded app.apk\n"),
              "window created before forwarded file");
        sendOpenFile(bus, QStringLiteral("file:///home/user/open%20file.apk"));
        waitEvents(eventFile, "FILE /home/user/open file.apk\n");

        QDBusMessage intent = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/omp/RuntimeManager/IntentHandler1"),
            QStringLiteral("ru.omp.RuntimeManager.IntentHandler1"), QStringLiteral("HandleIntent"));
        QVariantMap params;
        params.insert(QStringLiteral("uri"), QStringLiteral("file:///home/user/from%20intent.apk"));
        intent << QStringLiteral("OpenURI") << params;
        const QDBusMessage intentReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(intent))).reply();
        check(intentReply.type() != QDBusMessage::ErrorMessage, "RuntimeManager intent reply");
        waitEvents(eventFile, "FILE /home/user/from intent.apk\n");
        sendOpenFile(bus, QStringLiteral("file:///home/user/invalid.txt"));
        waitEventCount(eventFile, "REJECTED\n", 2);
        check(read(eventFile).count("GUI\n") == 1, "rejected file does not create another manager window");
        QThread::msleep(1800);
        check(process.state() == QProcess::Running, "shown manager does not use background idle timeout");
        process.terminate();
        check(process.waitForFinished(5000), "stop test manager");
    }
    {
        QProcess process;
        startServer(process, eventFile, true);
        check(ManagerLifecycle::forwardToRunningManager(QStringList(), &error) == ManagerLifecycle::Failed &&
              !error.isEmpty(), "GUI startup failure is reported to caller");
        check(process.waitForFinished(3000), "failed GUI leaves background idle timer active");
    }
    std::puts("Manager lifecycle tests passed");
    return 0;
}
