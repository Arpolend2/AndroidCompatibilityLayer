#include "../manager_lifecycle.h"
#include "../single_instance_lock.h"
#include "../standalone_diagnostic_registry.h"
#include "../standalone_microg_diagnostic.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QDBusMessage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" void megler_text_commit(const char *) {}
extern "C" void megler_text_key(int) {}
extern "C" void megler_keyboard_closed(void) {}
extern "C" void megler_keyboard_height(int) {}

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
        connection.send(message.createReply(QVariantList() << executable << key));
        return true;
    }
    QString sender;
    QString executable = QStringLiteral("/test/client");
    QString key = QStringLiteral("test-key");
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

QDBusPendingCall waitFor(const QDBusPendingCall &call, int timeoutMilliseconds = 5000)
{
    QDBusPendingCallWatcher watcher(call);
    if (!watcher.isFinished()) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMilliseconds);
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
    QDBusConnection bus = QDBusConnection::sessionBus();
    check(waitForOwner(bus, false), "activation starts without a manager");
    QString provider, error;
    check(ManagerLifecycle::forwardToRunningManager(QStringList(), &error) == ManagerLifecycle::NoOwner,
          "normal launch does not auto-activate even with an installed service");
    check(waitForOwner(bus, false), "normal launch left service inactive");
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
    {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString id = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
        QTemporaryDir rpmDirectory;
        check(rpmDirectory.isValid() && QDir().mkpath(root), "create activation diagnostic registry");
        QFile rpm(rpmDirectory.path() + QLatin1Char('/') + id + QStringLiteral("-1.0.0-1.x86_64.rpm"));
        check(rpm.open(QIODevice::WriteOnly), "create activation exported RPM");
        rpm.close();
        check(StandaloneDiagnosticRegistry::save(root, rpm.fileName(), QStringLiteral("org.example.app")),
              "register activation client");
        IdentityFixture identity;
        identity.executable = QStringLiteral("/usr/bin/") + id;
        check(bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity), "register activation identity");
        check(bus.registerService(QStringLiteral("ru.omp.am")), "register activation identity service");
        QDBusMessage ping = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("DiagnosticPing"));
        const QDBusMessage pingReply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
        check(pingReply.type() == QDBusMessage::ReplyMessage &&
              pingReply.arguments().value(0).toString() == QLatin1String("synthetic-ok") &&
              identity.sender == bus.baseService(), "diagnostic cold-activates manager for known sender");
        check(read(eventFile).isEmpty(), "diagnostic activation does not open a window");
        bus.unregisterService(QStringLiteral("ru.omp.am"));
        bus.unregisterObject(QStringLiteral("/ru/omp/am"));
        check(QFile::remove(QDir(StandaloneDiagnosticRegistry::directory(root)).filePath(id)),
              "remove activation test registry entry");
        check(QDir().rmdir(StandaloneDiagnosticRegistry::directory(root)), "remove activation registry");
        check(QDir().rmdir(root), "remove activation app data directory");
        check(QDir().rmdir(QFileInfo(root).absolutePath()), "remove activation organization data directory");
    }
#endif
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

int actualManager(const QString &binary, bool transition)
{
    QTemporaryDir directory;
    check(directory.isValid(), "create isolated actual manager configuration");
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString platform = QString::fromLocal8Bit(qgetenv("ACL_MANAGER_TEST_PLATFORM"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       transition && !platform.isEmpty() ? platform : QStringLiteral("minimal"));
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
    const QString id = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    const QString root = QDir(directory.path()).filePath(QStringLiteral("ru.androidcompatibilitylayer/appmanager"));
    check(QDir().mkpath(root), "create actual manager registry directory");
    QFile rpm(QDir(directory.path()).filePath(id + QStringLiteral("-1.0.0-1.x86_64.rpm")));
    check(rpm.open(QIODevice::WriteOnly), "create actual manager exported RPM");
    rpm.close();
    check(StandaloneDiagnosticRegistry::save(root, rpm.fileName(), QStringLiteral("org.example.app")),
          "register actual manager diagnostic client");
    identity.executable = QStringLiteral("/usr/bin/") + id;
    QDBusMessage resolve = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("ResolveCaller"));
    QDBusPendingReply<QStringList> resolved(waitFor(bus.asyncCall(resolve)));
    check(resolved.isError() && resolved.error().type() == QDBusError::AccessDenied,
          "manager refuses registry-only identity mapping");
    QDBusMessage open = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("OpenChannel"));
    QDBusPendingReply<QDBusUnixFileDescriptor> unavailable(waitFor(bus.asyncCall(open)));
    check(unavailable.isError() && unavailable.error().type() == QDBusError::NotSupported,
          "manager without a microG backend rejects channel requests");
    QDBusMessage openClient = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("OpenClientChannel"));
    QDBusPendingReply<> clientUnavailable(waitFor(bus.asyncCall(openClient)));
    check(clientUnavailable.isError() && clientUnavailable.error().type() == QDBusError::NotSupported,
          "manager without a microG backend rejects identity channel requests");
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("DiagnosticPing"));
    const QDBusMessage response = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
    check(response.type() == QDBusMessage::ReplyMessage &&
          response.arguments().value(0).toString() == QLatin1String("synthetic-ok"),
          "actual manager serves diagnostic for registered sender");
#endif
    check(process.state() == QProcess::Running, "actual manager stays running without a window");
    if (transition) {
        const QDBusReply<uint> originalPid = bus.interface()->servicePid(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        bool ok = originalPid.isValid();
        for (int attempt = 0; attempt < 2 && ok; ++attempt) {
            QProcess launch;
            launch.setProcessEnvironment(environment);
            launch.start(binary, QStringList());
            const bool finished = launch.waitForFinished(15000);
            if (!finished) {
                launch.kill();
                launch.waitForFinished(5000);
            }
            const bool managerExited = process.waitForFinished(500);
            const QDBusReply<uint> owner = bus.interface()->servicePid(
                QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
            ok = finished && launch.exitStatus() == QProcess::NormalExit && launch.exitCode() == 0 &&
                 !managerExited && owner.isValid() && owner.value() == originalPid.value();
            if (!ok) {
                std::fprintf(stderr, "Manager transition attempt %d failed: launcher exit=%d status=%d; "
                             "manager exited=%d exit=%d status=%d\nlauncher stderr:\n%s\nmanager stderr:\n%s\n",
                             attempt + 1, launch.exitCode(), int(launch.exitStatus()), managerExited,
                             process.exitCode(), int(process.exitStatus()),
                             launch.readAllStandardError().constData(), process.readAllStandardError().constData());
            }
        }
        if (!ok) {
            if (process.state() != QProcess::NotRunning) {
                process.terminate();
                process.waitForFinished(5000);
            }
            bus.unregisterService(QStringLiteral("ru.omp.am"));
            bus.unregisterObject(QStringLiteral("/ru/omp/am"));
            return 1;
        }
    }
    process.terminate();
    check(process.waitForFinished(5000), "stop actual test manager");
    bus.unregisterService(QStringLiteral("ru.omp.am"));
    bus.unregisterObject(QStringLiteral("/ru/omp/am"));
    std::puts("Actual manager background startup test passed");
    return 0;
}

int sharedMicrogWorkerIntegration(const QString &workerExecutable, const QString &apk,
                                 const QString &androidRoot)
{
    QTemporaryDir directory;
    check(directory.isValid(), "create isolated shared microG integration directory");
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    qputenv("XDG_DATA_HOME", directory.path().toLocal8Bit());
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("none"));
    settings.sync();
    check(settings.status() == QSettings::NoError, "select integration microG provider");

    const QString identityDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    check(QDir().mkpath(identityDirectory), "create isolated caller registry");
    const QString appId = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    const QString rpmPath = QDir(directory.path()).filePath(appId + QStringLiteral("-1.0.0-1.x86_64.rpm"));
    QFile rpm(rpmPath);
    check(rpm.open(QIODevice::WriteOnly), "create exported caller fixture");
    rpm.close();
    check(StandaloneDiagnosticRegistry::save(identityDirectory, rpmPath, QStringLiteral("example.client1")),
          "register exported caller package");

    const QString dataDirectory = QDir(directory.path()).filePath(QStringLiteral("worker-data"));
    const QString appsDirectory = QDir(directory.path()).filePath(QStringLiteral("worker-apps"));
    check(QDir().mkpath(dataDirectory) && QDir().mkpath(appsDirectory),
          "create isolated worker directories");
    QFile apps(QDir(appsDirectory).filePath(QStringLiteral("apps.txt")));
    check(apps.open(QIODevice::WriteOnly | QIODevice::Text), "create worker package list");
    const QByteArray appList = "example.client1\t\tClient\t1.0\t1\n";
    check(apps.write(appList) == appList.size(), "write worker package list");
    apps.close();

    QDBusConnection bus = QDBusConnection::sessionBus();
    IdentityFixture identity;
    identity.executable = QStringLiteral("/usr/bin/") + appId;
    check(bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity),
          "register fixture identity object");
    check(bus.registerService(QStringLiteral("ru.omp.am")), "register fixture identity service");

    ManagerLifecycle lifecycle([]() { return false; });
    const QStringList workerArguments = QStringList()
        << QStringLiteral("--apk=") + QFileInfo(apk).absoluteFilePath()
        << QStringLiteral("--data-dir=") + dataDirectory
        << QStringLiteral("--android-root=") + QFileInfo(androidRoot).absoluteFilePath()
        << QStringLiteral("--apps-dir=") + appsDirectory;
    QString error;
    check(lifecycle.start(true, &error), "start manager integration service before opt-in");
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();
    check(settings.status() == QSettings::NoError, "opt in to microG after manager startup");
    lifecycle.refreshSharedMicrogProvider();
    check(lifecycle.configureSharedMicrogWorker(
              QFileInfo(workerExecutable).absoluteFilePath(), workerArguments,
              ManagerLifecycle::registryPackageResolver(identityDirectory),
              [](const QString &apkPath, const QString &package, QString *metadataError) {
                  if (metadataError) metadataError->clear();
                  if (package != QLatin1String("example.client1")) {
                      if (metadataError) *metadataError = QStringLiteral("unexpected package");
                      return QByteArray();
                  }
                  if (apkPath == QLatin1String("/fixture/mismatch.apk"))
                      return QByteArray("{\"package\":\"example.other\",\"activity\":\"\",\"label\":\"Other\","
                                        "\"versionName\":\"1\",\"versionCode\":\"1\",\"activities\":{"
                                        "\"package\":\"example.other\",\"activities\":[],\"certificates\":[\"3000\"]}}");
                  if (apkPath == QLatin1String("/fixture/client-v2.apk"))
                      return QByteArray("{\"package\":\"example.client1\",\"activity\":\"\",\"label\":\"Client v2\","
                                        "\"versionName\":\"2.0\",\"versionCode\":\"2\",\"activities\":{"
                                        "\"package\":\"example.client1\",\"activities\":[],\"certificates\":[\"3000\"]}}");
                  if (apkPath == QLatin1String("/fixture/missing.apk")) {
                      if (metadataError) *metadataError = QStringLiteral("APK unavailable");
                      return QByteArray();
                  }
                  if (apkPath == QLatin1String("/fixture/oversize.apk"))
                      return QByteArray(1024 * 1024 + 1, 'x');
                  return QByteArray("{\"package\":\"example.client1\",\"activity\":\"\",\"label\":\"Client live\","
                                    "\"versionName\":\"1.0\",\"versionCode\":\"1\",\"activities\":{"
                                    "\"package\":\"example.client1\",\"activities\":[],\"certificates\":[\"3000\"]}}");
              }, &error),
          "configure manager-derived resolver and production worker after opt-in");

    int stream = -1;
    const QString clientName = QStringLiteral("shared-microg-worker-integration-client");
    {
        QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, clientName);
        check(client.isConnected(), "connect dedicated integration client");
        QDBusMessage resolve = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("ResolveCaller"));
        QDBusPendingReply<QStringList> identityReply(waitFor(client.asyncCall(resolve, 120000), 120000));
        check(!identityReply.isError() && identityReply.value().size() == 4 &&
              identityReply.value().at(0) == client.baseService() &&
              identityReply.value().at(1) == QLatin1String("example.client1") &&
              identityReply.value().at(2).toInt() >= 11000 &&
              identityReply.value().at(3).toInt() >= 50000,
              "resolve fixture caller through IdentifyByBusName and export registry");

        QDBusMessage missingPath = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        QDBusPendingReply<> rejectedMissingPath(waitFor(client.asyncCall(missingPath, 120000), 120000));
        check(rejectedMissingPath.isError() &&
              rejectedMissingPath.error().type() == QDBusError::InvalidArgs,
              "reject a client APK request without a path");

        QDBusMessage missingApk = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        missingApk << QStringLiteral("/fixture/missing.apk");
        QDBusPendingReply<> rejectedMissingApk(waitFor(client.asyncCall(missingApk, 120000), 120000));
        check(rejectedMissingApk.isError() && rejectedMissingApk.error().type() == QDBusError::Failed,
              "report a missing client APK instead of opening a metadata-free channel");

        QDBusMessage oversize = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        oversize << QStringLiteral("/fixture/oversize.apk");
        QDBusPendingReply<> rejectedOversize(waitFor(client.asyncCall(oversize, 120000), 120000));
        check(rejectedOversize.isError() && rejectedOversize.error().type() == QDBusError::Failed,
              "reject oversized APK metadata before opening a worker channel");

        QDBusMessage mismatch = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        mismatch << QStringLiteral("/fixture/mismatch.apk");
        QDBusPendingReply<QDBusUnixFileDescriptor, int, int, QString> rejectedMetadata(
            waitFor(client.asyncCall(mismatch, 120000), 120000));
        check(rejectedMetadata.isError() && rejectedMetadata.error().type() == QDBusError::Failed,
              "reject APK metadata whose package differs from the identified client");

        QDBusMessage open = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        open << QStringLiteral("/fixture/client.apk");
        const QDBusMessage opened = QDBusPendingCallWatcher(
            waitFor(client.asyncCall(open, 120000), 120000)).reply();
        const QList<QVariant> values = opened.arguments();
        check(opened.type() == QDBusMessage::ReplyMessage && values.size() == 4 &&
              values.at(0).canConvert<QDBusUnixFileDescriptor>(),
              "manager returns channel only after production worker ACK");
        check(values.at(1).type() == QVariant::Int &&
              values.at(1).toInt() == identityReply.value().at(2).toInt() &&
              values.at(2).type() == QVariant::Int &&
              values.at(2).toInt() == identityReply.value().at(3).toInt() &&
              values.at(3).toString() == identityReply.value().at(1),
              "channel reply carries the manager-resolved identity sent to the worker");
        QDBusUnixFileDescriptor descriptor = values.at(0).value<QDBusUnixFileDescriptor>();
        stream = takeSharedMicrogStream(&descriptor, &error);
        check(stream >= 0, "take manager-issued stream descriptor");

        QFile publishedList(QDir(appsDirectory).filePath(QStringLiteral("apps.txt")));
        check(publishedList.open(QIODevice::ReadOnly) &&
              publishedList.readAll().contains("example.client1\t\tClient live\t1.0\t1\n"),
              "publish live APK package metadata for the shared worker");
        QFile publishedActivities(QDir(appsDirectory).filePath(
            QStringLiteral("example.client1/activities.json")));
        check(publishedActivities.open(QIODevice::ReadOnly) &&
              publishedActivities.readAll().contains("\"certificates\":[\"3000\"]"),
              "publish the live APK certificate in the worker metadata");

        const QString secondClientName = QStringLiteral("shared-microg-worker-conflict-client");
        QDBusConnection secondClient = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, secondClientName);
        check(secondClient.isConnected(), "connect second fixture client");
        QDBusMessage conflict = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            QStringLiteral("OpenClientChannelWithApk"));
        conflict << QStringLiteral("/fixture/client-v2.apk");
        QDBusPendingReply<QDBusUnixFileDescriptor, int, int, QString> rejectedVersion(
            waitFor(secondClient.asyncCall(conflict, 120000), 120000));
        check(rejectedVersion.isError() && rejectedVersion.error().type() == QDBusError::Failed,
              "reject concurrent conflicting versions of the same package");
        QDBusConnection::disconnectFromBus(secondClientName);
        QDBusConnection::disconnectFromBus(clientName);
    }

    QElapsedTimer elapsed;
    elapsed.start();
    bool eof = false;
    while (!eof && elapsed.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        struct pollfd streamEvent = { stream, POLLIN | POLLHUP, 0 };
        if (::poll(&streamEvent, 1, 25) > 0) {
            char byte = 0;
            const ssize_t count = ::read(stream, &byte, 1);
            if (count == 0) eof = true;
            else check(count < 0, "discard no unexpected stream payload during teardown");
        }
    }
    ::close(stream);
    check(eof, "client disconnect revokes channel and stops worker bridge");
    QFile restoredList(QDir(appsDirectory).filePath(QStringLiteral("apps.txt")));
    check(restoredList.open(QIODevice::ReadOnly) && restoredList.readAll() == appList,
          "restore the library package list after the final client disconnects");
    check(!QFileInfo(QDir(appsDirectory).filePath(
              QStringLiteral("example.client1/activities.json"))).exists(),
          "remove temporary APK metadata after the final client disconnects");
    bus.unregisterService(QStringLiteral("ru.omp.am"));
    bus.unregisterObject(QStringLiteral("/ru/omp/am"));
    std::puts("Manager→supervisor→production self-spawn→microG worker integration passed");
    return 0;
}
// Test-only stand-in for Aurora's identification service: reports the sender's real executable.
class ProcessIdentityFixture : public QDBusVirtualObject
{
public:
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("ru.omp.am.Identify") ||
            message.member() != QLatin1String("IdentifyByBusName")) return false;
        const QString sender = message.arguments().value(0).toString();
        const QDBusReply<uint> pid = connection.interface()->servicePid(sender);
        const QString executable = pid.isValid()
            ? QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid.value())) : QString();
        if (message.arguments().size() != 1 || executable.isEmpty()) {
            connection.send(message.createErrorReply(QDBusError::Failed, QStringLiteral("unknown sender")));
            return true;
        }
        connection.send(message.createReply(QVariantList() << executable << QStringLiteral("test-key")));
        return true;
    }
};

// Test-only manager: argv = apk android-root data-dir apps-dir appId=package...
// Runs until stdin closes. Settings, registry and UID allocation use a fresh temporary directory.
int sharedMicrogManagerFixture(QCoreApplication &app, const QStringList &args)
{
    const QString workerExecutable =
        QString::fromLocal8Bit(qgetenv("ACL_SHARED_MICROG_MANAGER_EXECUTABLE"));
    check(args.size() >= 5 && QFileInfo(workerExecutable).isExecutable(),
          "fixture arguments: apk android-root data-dir apps-dir appId=package...; "
          "ACL_SHARED_MICROG_MANAGER_EXECUTABLE set");
    QTemporaryDir directory;
    check(directory.isValid(), "create fixture configuration directory");
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    qputenv("XDG_DATA_HOME", directory.path().toLocal8Bit());
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();
    check(settings.status() == QSettings::NoError, "select fixture microG provider");
    const QString identityDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    check(QDir().mkpath(identityDirectory), "create fixture registry");
    for (int i = 4; i < args.size(); ++i) {
        const int separator = args.at(i).indexOf(QLatin1Char('='));
        check(separator > 0, "fixture mapping is appId=package");
        const QString rpmPath = QDir(directory.path()).filePath(
            args.at(i).left(separator) + QStringLiteral("-1.0.0-1.x86_64.rpm"));
        QFile rpm(rpmPath);
        check(rpm.open(QIODevice::WriteOnly), "create fixture exported RPM");
        rpm.close();
        check(StandaloneDiagnosticRegistry::save(identityDirectory, rpmPath, args.at(i).mid(separator + 1)),
              "register fixture caller package");
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    ProcessIdentityFixture identity;
    check(bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity), "register fixture identity object");
    check(bus.registerService(QStringLiteral("ru.omp.am")), "register fixture identity service");
    ManagerLifecycle lifecycle([]() { return false; });
    QString error;
    check(lifecycle.start(true, &error), "start fixture manager service");
    const QStringList workerArguments = QStringList()
        << QStringLiteral("--apk=") + QFileInfo(args.at(0)).absoluteFilePath()
        << QStringLiteral("--android-root=") + QFileInfo(args.at(1)).absoluteFilePath()
        << QStringLiteral("--data-dir=") + QFileInfo(args.at(2)).absoluteFilePath()
        << QStringLiteral("--apps-dir=") + QFileInfo(args.at(3)).absoluteFilePath();
    check(lifecycle.configureSharedMicrogWorker(
              QFileInfo(workerExecutable).absoluteFilePath(), workerArguments,
              ManagerLifecycle::registryPackageResolver(identityDirectory), &error),
          "configure fixture worker backend");
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&app, &input]() {
        char buffer[64];
        if (::read(STDIN_FILENO, buffer, sizeof(buffer)) <= 0) {
            input.setEnabled(false);
            app.quit();
        }
    });
    std::puts("SHARED_MICROG_MANAGER_FIXTURE_READY");
    std::fflush(stdout);
    const int result = app.exec();
    lifecycle.disableSharedMicrogWorker();
    bus.unregisterService(QStringLiteral("ru.omp.am"));
    bus.unregisterObject(QStringLiteral("/ru/omp/am"));
    return result;
}
}

int lockFile(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return -1;
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 1;
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool otherProcessCanLock(const QString &path)
{
    const int fd = lockFile(path);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

void step(const char *line)
{
    std::puts(line);
    std::fflush(stdout);
    char buffer[16];
    if (!std::fgets(buffer, sizeof(buffer), stdin)) std::exit(1);
}

// Every descriptor this process has open on path must be close-on-exec, so QProcess children
// (adb, guests) never keep the launcher lock file open.
bool descriptorsCloseOnExec(const QString &path)
{
    const QDir fds(QStringLiteral("/proc/self/fd"));
    int found = 0;
    for (const QString &name : fds.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot)) {
        if (QDir::cleanPath(QFile::symLinkTarget(fds.filePath(name))) != QDir::cleanPath(path)) continue;
        const int flags = fcntl(name.toInt(), F_GETFD);
        if (flags < 0 || !(flags & FD_CLOEXEC)) return false;
        ++found;
    }
    return found > 0;
}

bool waitHeld(QCoreApplication &app, SingleInstanceLock &lock, int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!lock.held() && timer.elapsed() < milliseconds) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return lock.held();
}

// Mirrors mapplauncherd: the booster locks inside the process that becomes the app and forgets the fd.
// Driven by the parent: adopt, windowless release, show while a forwarding launch holds the lock,
// hide -> show, show while the lock stays busy past the retry budget, then a later show.
int singleInstanceHolder(QCoreApplication &app, const QString &path)
{
    if (lockFile(path) < 0) return 1;
    SingleInstanceLock lock(nullptr, 50, 20);
    if (!lock.adoptInherited() || lock.path() != QDir::cleanPath(path) || !lock.held()) return 1;
    step(descriptorsCloseOnExec(path) ? "ADOPTED" : "ADOPTED-INHERITABLE");
    lock.setHeld(false);
    step(lock.held() ? "STILL-HELD" : "RELEASED");
    lock.setHeld(true);
    const bool held = waitHeld(app, lock, 5000);
    step(!held ? "NOT-HELD" : descriptorsCloseOnExec(path) ? "HELD" : "HELD-INHERITABLE");
    lock.setHeld(false); // window hidden
    step(lock.held() ? "STILL-HELD" : "HIDDEN");
    lock.setHeld(true); // window shown again
    step(lock.held() ? "SHOWN" : "NOT-HELD");
    lock.setHeld(false);
    step(lock.held() ? "STILL-HELD" : "HIDDEN");
    lock.setHeld(true);
    lock.setHeld(true); // repeated show while retrying must not restart the budget
    const bool exhausted = !waitHeld(app, lock, 3000) && !lock.retrying();
    step(exhausted ? "EXHAUSTED" : "UNEXPECTED");
    lock.setHeld(true);
    step(waitHeld(app, lock, 2000) ? "RECOVERED" : "NOT-HELD");
    return 0;
}

QByteArray readLine(QProcess &process)
{
    if (!process.canReadLine()) process.waitForReadyRead(6000);
    return process.readLine().trimmed();
}

void advance(QProcess &process)
{
    process.write("\n");
    check(process.waitForBytesWritten(5000), "advance single-instance holder");
}

void testSingleInstanceLock(const QString &root)
{
    const QString lockDir = root + QStringLiteral("/single-instance-locks/usr/bin/ru.androidcompatibilitylayer.appmanager");
    check(QDir().mkpath(lockDir), "create single-instance lock directory");
    const QString path = lockDir + QStringLiteral("/instance.lock");
    QProcess holder;
    holder.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    holder.start(QCoreApplication::applicationFilePath(),
                 QStringList() << QStringLiteral("--single-instance-holder") << path);
    check(holder.waitForStarted(5000), "start single-instance holder");
    check(readLine(holder) == "ADOPTED", "manager adopts the launcher lock and marks it close-on-exec");
    check(!otherProcessCanLock(path), "adopted lock still blocks other launches");
    advance(holder);
    check(readLine(holder) == "RELEASED", "windowless manager releases the lock");
    check(otherProcessCanLock(path), "a launch can reach a forwarding process while no window exists");
    int forwarder = lockFile(path);
    check(forwarder >= 0, "forwarding launch holds the lock");
    advance(holder);
    check(!holder.waitForReadyRead(300), "manager waits while the forwarding launch holds the lock");
    ::close(forwarder);
    check(readLine(holder) == "HELD", "manager reacquires a close-on-exec lock once its window is shown");
    check(!otherProcessCanLock(path), "shown manager blocks duplicate launches again");
    advance(holder);
    check(readLine(holder) == "HIDDEN", "hiding the window releases the lock");
    check(otherProcessCanLock(path), "launch reaches a forwarding process after hide");
    advance(holder);
    check(readLine(holder) == "SHOWN", "showing the window again reacquires the lock");
    check(!otherProcessCanLock(path), "lock blocks duplicate launches after hide -> show");
    advance(holder);
    check(readLine(holder) == "HIDDEN", "second hide releases the lock");
    forwarder = lockFile(path);
    check(forwarder >= 0, "long forwarding launch holds the lock");
    advance(holder);
    check(readLine(holder) == "EXHAUSTED", "busy lock stops retrying after the retry budget");
    ::close(forwarder);
    advance(holder);
    check(readLine(holder) == "RECOVERED", "a later show reacquires after exhausted retries");
    check(!otherProcessCanLock(path), "recovered lock blocks duplicate launches");
    advance(holder);
    check(holder.waitForFinished(5000) && holder.exitCode() == 0, "single-instance holder exits");
    check(otherProcessCanLock(path), "lock is released on exit");
    std::puts("Single-instance lock handover OK");
}

static void fake32Write(char *buffer, quint32 value)
{
    buffer[0] = static_cast<char>(value >> 24);
    buffer[1] = static_cast<char>(value >> 16);
    buffer[2] = static_cast<char>(value >> 8);
    buffer[3] = static_cast<char>(value);
}

static void fake64Write(char *buffer, quint64 value)
{
    for (int i = 7; i >= 0; --i) {
        buffer[i] = static_cast<char>(value);
        value >>= 8;
    }
}

static bool fakeSendControl(int fd, quint32 type, quint64 channel)
{
    char packet[28] = {'M','G','R','1'};
    fake32Write(packet + 4, type);
    fake64Write(packet + 8, channel);
    if (type == 8 || type == 9) fake64Write(packet + 16, 1);
    return ::send(fd, packet, sizeof(packet), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(packet));
}

static quint32 fake32Read(const char *data)
{
    return (quint32(static_cast<unsigned char>(data[0])) << 24) |
           (quint32(static_cast<unsigned char>(data[1])) << 16) |
           (quint32(static_cast<unsigned char>(data[2])) << 8) |
           quint32(static_cast<unsigned char>(data[3]));
}

static bool fakeSizedFrame(quint64 channel, quint64 sequence, quint32 width, quint32 height)
{
    const quint32 total = width * height * 4;
    QByteArray packet(56 + 60 * 1024, char(0xff));
    for (quint32 offset = 0; offset < total;) {
        const quint32 length = qMin(quint32(60 * 1024), total - offset);
        std::memcpy(packet.data(), "MGUI", 4);
        fake32Write(packet.data() + 4, 2);
        fake64Write(packet.data() + 8, channel);
        fake64Write(packet.data() + 16, sequence);
        fake32Write(packet.data() + 24, width);
        fake32Write(packet.data() + 28, height);
        fake32Write(packet.data() + 32, width * 4);
        fake32Write(packet.data() + 36, total);
        fake32Write(packet.data() + 40, offset);
        fake32Write(packet.data() + 44, length);
        fake64Write(packet.data() + 48, 1);
        if (::send(4, packet.constData(), 56 + length, MSG_NOSIGNAL) !=
            static_cast<ssize_t>(56 + length)) return false;
        offset += length;
    }
    return true;
}

int fakeMicrogQmlWorker()
{
    const QString geometryLog = QString::fromLocal8Bit(qgetenv("ACL_MICROG_GEOMETRY_FILE"));
    quint64 sequence = 1;
    char ready[28] = {'M','G','R','1'};
    fake32Write(ready + 4, 1);
    if (::send(3, ready, sizeof(ready), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(sizeof(ready))) return 1;

    char packet[28 + 255];
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec vector = {packet, sizeof(packet)};
    struct msghdr message = {};
    for (;;) {
        std::memset(&control, 0, sizeof(control));
        std::memset(&message, 0, sizeof(message));
        vector.iov_base = packet;
        vector.iov_len = sizeof(packet);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        ssize_t count = ::recvmsg(3, &message, MSG_CMSG_CLOEXEC);
        if (count < 28) return 2;
        quint32 type = (static_cast<unsigned char>(packet[4]) << 24) |
                       (static_cast<unsigned char>(packet[5]) << 16) |
                       (static_cast<unsigned char>(packet[6]) << 8) |
                       static_cast<unsigned char>(packet[7]);
        quint64 channel = 0;
        for (int i = 0; i < 8; ++i)
            channel = (channel << 8) | static_cast<unsigned char>(packet[8 + i]);
        if (type == 2) {
            char frame[56 + 16];
            std::memcpy(frame, "MGUI", 4);
            fake32Write(frame + 4, 2);
            fake64Write(frame + 8, channel);
            fake64Write(frame + 16, 1);
            fake32Write(frame + 24, 2);
            fake32Write(frame + 28, 2);
            fake32Write(frame + 32, 8);
            fake32Write(frame + 36, 16);
            fake32Write(frame + 40, 0);
            fake32Write(frame + 44, 16);
            fake64Write(frame + 48, 1);
            for (int i = 0; i < 16; ++i)
                frame[56 + i] = static_cast<char>(i + 1);
            if (::send(4, frame, sizeof(frame), MSG_NOSIGNAL) !=
                static_cast<ssize_t>(sizeof(frame))) return 3;
            if (!fakeSendControl(3, 8, channel)) return 4;
            if (!fakeSendControl(3, 5, channel)) return 5;
            continue;
        }
        if (type == 4) {
            fakeSendControl(3, 5, 0);
            return 0;
        }
        if (type == 3) {
            fakeSendControl(3, 5, channel);
            continue;
        }
        if (!geometryLog.isEmpty() && type == 21 && count == 44) {
            if (fake32Read(packet + 28) != 0 || fake32Read(packet + 32) != 1) return 6;
            const quint32 width = fake32Read(packet + 16), height = fake32Read(packet + 20);
            if (!width || width > 2048 || !height || height > 2048) return 6;
            if (qgetenv("ACL_MICROG_SKIP_RESIZE_FRAME").isEmpty() &&
                !fakeSizedFrame(channel, ++sequence, width, height)) return 7;
            continue;
        }
        if (!geometryLog.isEmpty() && type == 12 && count == 44) {
            if (fake32Read(packet + 28) != 0 || fake32Read(packet + 32) != 1) return 6;
            append(geometryLog, QByteArray::number(fake32Read(packet + 16)) + ' ' +
                   QByteArray::number(fake32Read(packet + 36)) + ' ' +
                   QByteArray::number(fake32Read(packet + 40)) + '\n');
            continue;
        }
        if (count >= 28) continue;
    }
}

int managerMicrogQmlClient(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return 21;
    QDBusMessage open = QDBusMessage::createMethodCall(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
        QStringLiteral("OpenChannel"));
    QDBusPendingReply<QDBusUnixFileDescriptor> opened(
        waitFor(bus.asyncCall(open, 10000), 10000));
    if (opened.isError() || !opened.value().isValid()) {
        std::fprintf(stderr, "open channel failed: %s / %s\n",
                     qPrintable(opened.error().name()), qPrintable(opened.error().message()));
        return 22;
    }
    std::puts("QML client opened pre-ACK channel");
    if (!qgetenv("ACL_MICROG_GEOMETRY_FILE").isEmpty())
        (void)std::getchar();
    return 0;
}

int managerMicrogQmlFrameFixture(int argc, char **argv)
{
    const bool matching = argc == 2 && QByteArray(argv[1]) == "--manager-microg-matching-frame-fixture";
    const bool geometry = matching || (argc == 2 && QByteArray(argv[1]) == "--manager-microg-geometry-fixture");
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager-qml-frame"));

    QTemporaryDir directory;
    if (!directory.isValid()) return 10;
    const QString geometryLog = QDir(directory.path()).filePath(QStringLiteral("geometry-events"));
    if (geometry) qputenv("ACL_MICROG_GEOMETRY_FILE", geometryLog.toLocal8Bit());
    if (matching) qputenv("ACL_MICROG_SKIP_RESIZE_FRAME", "1");
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    qputenv("XDG_DATA_HOME", directory.path().toLocal8Bit());
    const QString identityDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(identityDirectory)) return 10;
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
    settings.sync();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return 11;
    IdentityFixture identity;
    identity.executable = QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    if (!bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) ||
        !bus.registerService(QStringLiteral("ru.omp.am"))) return 12;

    ManagerLifecycle lifecycle([]() { return true; });
    QString error;
    if (!lifecycle.start(true, &error)) {
        std::fprintf(stderr, "manager start failed: %s\n", qPrintable(error));
        return 13;
    }
    if (!lifecycle.configureSharedMicrogWorker(
            QFileInfo(app.applicationFilePath()).absoluteFilePath(),
            QStringList(),
            [](const QString &, const QString &, QString *) {
                return QStringLiteral("org.example.app");
            }, &error)) {
        std::fprintf(stderr, "worker configure failed: %s\n", qPrintable(error));
        return 14;
    }

    QProcess client;
    client.start(app.applicationFilePath(),
                 QStringList() << QStringLiteral("--manager-microg-qml-client"));
    if (!client.waitForStarted(5000)) {
        std::fprintf(stderr, "QML client process did not start\n");
        return 15;
    }

    qmlRegisterType<SharedMicrogAuthSurface>(
        "AndroidCompatibilityLayer.MicroG", 1, 0, "SharedMicrogAuthSurface");
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("lifecycle"), &lifecycle);
    QQmlComponent component(&engine);
    component.setData(
        "import QtQuick 2.2\n"
        "import AndroidCompatibilityLayer.MicroG 1.0\n"
        "Item {\n"
        "  width: 800\n"
        "  height: 480\n"
        "  SharedMicrogAuthSurface { objectName: 'authSurface'; anchors.fill: parent; host: lifecycle }\n"
        "  Text { objectName: 'waitingLabel'; text: 'Waiting for the Google sign-in screen...'; "
        "visible: !lifecycle.authenticationFrameAvailable }\n"
        "}\n",
        QUrl());
    if (component.isError()) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 16;
    }
    QObject *root = component.create();
    if (!root) return 17;
    QPointer<QObject> rootPointer(root);
    QQuickWindow window;
    QQuickItem *rootItem = qobject_cast<QQuickItem *>(root);
    if (geometry) {
        if (!rootItem) return 22;
        rootItem->setParentItem(window.contentItem());
        rootItem->setWidth(matching ? 2 : 480);
        rootItem->setHeight(matching ? 2 : 800);
    }

    QElapsedTimer elapsed;
    elapsed.start();
    while (!lifecycle.authenticationFrameAvailable() && elapsed.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!geometry && client.state() == QProcess::Running)
        client.waitForFinished(5000);
    if (!geometry && (client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0)) {
        std::fprintf(stderr, "QML client failed: %s\n",
                     qPrintable(client.readAllStandardError()));
        delete rootPointer.data();
        return 21;
    }
    if (!lifecycle.authenticationFrameAvailable()) {
        std::fprintf(stderr, "fixture frame unavailable; UI active=%d\n", lifecycle.authenticationUiActive());
        delete rootPointer.data();
        return 18;
    }
    QQuickItem *authSurface = rootPointer->findChild<QQuickItem*>(QStringLiteral("authSurface"));
    QObject *waiting = rootPointer->findChild<QObject*>(QStringLiteral("waitingLabel"));
    if (!authSurface || !waiting ||
        (!geometry && (lifecycle.authenticationFrame().width() != 2 ||
                       lifecycle.authenticationFrame().height() != 2)) ||
        waiting->property("visible").toBool()) {
        delete rootPointer.data();
        return 19;
    }
    quint64 nonzero = 0;
    const QImage frame = lifecycle.authenticationFrame();
    for (int y = 0; y < frame.height(); ++y) {
        const unsigned char *row = frame.constBits() + y * frame.bytesPerLine();
        for (int x = 0; x < frame.width() * 4; ++x)
            if (row[x]) ++nonzero;
    }
    if (!nonzero) {
        delete rootPointer.data();
        return 20;
    }
    if (matching) {
        if (lifecycle.authenticationFrame().size() != QSize(2, 2)) return 28;
        client.write("x\n");
        client.closeWriteChannel();
        if (!client.waitForFinished(5000) || client.exitCode() != 0) return 29;
        std::puts("Matching pre-ACK frame retained without another frame");
    } else if (geometry) {
        auto waitFrame = [&](const QSize &size) {
            QElapsedTimer timer;
            timer.start();
            while (lifecycle.authenticationFrame().size() != size && timer.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(1);
            }
            return lifecycle.authenticationFrame().size() == size;
        };
        auto touch = [&](const QPointF &point, bool expected) {
            QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(authSurface, &press);
            if (press.isAccepted() != expected) return false;
            if (expected) {
                QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(authSurface, &release);
            }
            return true;
        };
        auto noBars = [&]() {
            SharedMicrogAuthSurface *surface = qobject_cast<SharedMicrogAuthSurface *>(authSurface);
            if (!surface) return false;
            QImage image(int(surface->width()), int(surface->height()), QImage::Format_ARGB32);
            image.fill(Qt::black);
            QPainter painter(&image);
            surface->paint(&painter);
            painter.end();
            return image.pixel(0, 0) == 0xffffffffu &&
                   image.pixel(image.width() - 1, image.height() - 1) == 0xffffffffu;
        };
        if (!waitFrame(QSize(480, 800)) || !noBars() ||
            !touch(QPointF(240, 400), true)) return 23;
        rootItem->setWidth(800);
        rootItem->setHeight(480);
        if (!touch(QPointF(400, 240), false) || !waitFrame(QSize(800, 480)) ||
            !noBars() || !touch(QPointF(400, 240), true)) return 24;
        rootItem->setHeight(300);
        if (!touch(QPointF(400, 150), false) || !waitFrame(QSize(800, 300)) ||
            !noBars() || !touch(QPointF(400, 150), true)) return 25;
        QElapsedTimer timer;
        timer.start();
        QByteArray events;
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QFile file(geometryLog);
            if (file.open(QIODevice::ReadOnly)) events = file.readAll();
            if (events.count('\n') >= 6) break;
            QThread::msleep(1);
        } while (timer.elapsed() < 3000);
        if (events != "0 240 400\n2 240 400\n0 400 240\n2 400 240\n0 400 150\n2 400 150\n")
            return 26;
        client.write("x\n");
        client.closeWriteChannel();
        if (!client.waitForFinished(5000) || client.exitCode() != 0) return 27;
        std::puts("Manager/QML portrait, landscape and resized touch fixture passed");
    }
    if (!geometry) std::puts("Manager/QML pre-ACK frame fixture passed");
    delete rootPointer.data();
    return 0;
}

int main(int argc, char **argv)
{
    qputenv("AURORA_APP_ID", "ru.androidcompatibilitylayer.appmanager");
    qputenv("AURORA_APP_INSTANCE_ID", "ru.androidcompatibilitylayer.appmanager");
    if (argc >= 2 && QByteArray(argv[1]) == "--shared-microg-worker")
        return fakeMicrogQmlWorker();
    if (argc == 2 && QByteArray(argv[1]) == "--manager-microg-qml-client")
        return managerMicrogQmlClient(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--manager-microg-qml-frame-fixture")
        return managerMicrogQmlFrameFixture(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--manager-microg-geometry-fixture")
        return managerMicrogQmlFrameFixture(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--manager-microg-matching-frame-fixture")
        return managerMicrogQmlFrameFixture(argc, argv);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    if (argc == 3 && QByteArray(argv[1]) == "--single-instance-holder")
        return singleInstanceHolder(app, QString::fromLocal8Bit(argv[2]));
    if (argc == 4 && QByteArray(argv[1]) == "--server")
        return server(app, QString::fromLocal8Bit(argv[2]), QByteArray(argv[3]) == "fail",
                      QByteArray(argv[3]) == "auto", QByteArray(argv[3]) == "keep");
    if (argc == 3 && QByteArray(argv[1]) == "--activation-client")
        return activationClient(QString::fromLocal8Bit(argv[2]));
    if (argc == 3 && (QByteArray(argv[1]) == "--actual-manager" ||
                      QByteArray(argv[1]) == "--actual-manager-transition"))
        return actualManager(QString::fromLocal8Bit(argv[2]), QByteArray(argv[1]) == "--actual-manager-transition");
    if (argc == 2 && QByteArray(argv[1]) == "--shared-microg-worker-integration")
        return sharedMicrogWorkerIntegration(
            QString::fromLocal8Bit(qgetenv("ACL_SHARED_MICROG_MANAGER_EXECUTABLE")),
            QString::fromLocal8Bit(qgetenv("ACL_SHARED_MICROG_TEST_APK")),
            QString::fromLocal8Bit(qgetenv("ACL_SHARED_MICROG_TEST_ANDROID_ROOT")));
    if (argc >= 7 && QByteArray(argv[1]) == "--shared-microg-manager-fixture") {
        QStringList args;
        for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
        return sharedMicrogManagerFixture(app, args);
    }
    if (argc == 2 && QByteArray(argv[1]) == "--diagnostic-client") {
        const auto result = StandaloneMicrogDiagnostic::run(QDBusConnection::sessionBus());
        std::printf("owner=%s\nreply=%s\n", qPrintable(result.owner), qPrintable(result.response));
        return 0;
    }
    if (argc == 3 && QByteArray(argv[1]) == "--desktop") {
        QFile desktop(QString::fromLocal8Bit(argv[2]));
        check(desktop.open(QIODevice::ReadOnly), "open manager desktop");
        bool applicationSection = false;
        bool allowsMultipleInstances = false;
        bool singleCover = false;
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
            else if (!applicationSection && line == "X-Aurora-Single-Cover=yes")
                singleCover = true;
        }
        const QList<QByteArray> exports = exported.split(';');
        check(exports.contains(QByteArray("ru.androidcompatibilitylayer.appmanager.log")),
              "manager desktop exports log D-Bus interface");
        check(exec == QByteArray("/usr/bin/ru.androidcompatibilitylayer.appmanager --microg-service"),
              "manager desktop activates background manager");
        // A window-less --microg-service instance cannot answer the launcher's activation, so each
        // tap starts a short-lived instance that forwards to it; one cover keeps a single switcher entry.
        check(allowsMultipleInstances && singleCover,
              "manager desktop starts forwarding instances with a single cover");
        std::puts("Manager desktop contract OK");
        return 0;
    }

    QTemporaryDir directory;
    check(directory.isValid(), "create test directory");
    testSingleInstanceLock(directory.path());
    const QStringList uri{QStringLiteral("file:///tmp/sample.apk")};
    const QStringList malformed{QStringLiteral("file:///tmp/sample%u.apk")};
    const QStringList extra{QStringLiteral("%u"), uri.first()};
    check(ManagerLifecycle::launchFileArguments(uri) == uri &&
          ManagerLifecycle::launchFileArguments(malformed) == malformed &&
          ManagerLifecycle::launchFileArguments(extra) == extra,
          "startup only ignores the sole unexpanded desktop field code");
    qputenv("XDG_CONFIG_HOME", directory.path().toLocal8Bit());
    qputenv("XDG_DATA_HOME", directory.path().toLocal8Bit());
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
        const QDBusConnection invalid = QDBusConnection::connectToBus(
            QStringLiteral("unix:path=/nonexistent-acl-diagnostic-bus"), QStringLiteral("invalid-test-bus"));
        const auto result = StandaloneMicrogDiagnostic::run(invalid);
        check(result.owner.startsWith(QLatin1String("error:")) &&
              result.response == QLatin1String("not sent"), "bus failure is distinct from absent owner");
        QDBusConnection::disconnectFromBus(QStringLiteral("invalid-test-bus"));
    }
    {
        QProcess client;
        client.start(app.applicationFilePath(), QStringList() << QStringLiteral("--diagnostic-client"));
        check(client.waitForFinished(20000), "finish absent-owner diagnostic client");
        const QByteArray output = client.readAllStandardOutput();
        check(output.contains("owner=absent\n") && output.contains("reply=error:"),
              "client reports absent owner and activation failure separately");
    }

    {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString id = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
        const QString second = QStringLiteral("ru.androidcompatibilitylayer.Android1123456789abcdef01234567");
        check(QDir().mkpath(root), "create private registry root");
        const QString rpm = directory.path() + QLatin1Char('/') + id + QStringLiteral("-1.0.0-1.x86_64.rpm");
        QFile fixture(rpm);
        check(fixture.open(QIODevice::WriteOnly), "create exported RPM fixture");
        fixture.close();
        check(StandaloneDiagnosticRegistry::save(root, rpm, QStringLiteral("org.example.app")),
              "save exported mapping");
        check(StandaloneDiagnosticRegistry::save(root, rpm, QStringLiteral("org.example.app")),
              "repeat cached export");
        check(!StandaloneDiagnosticRegistry::save(root, rpm, QStringLiteral("org.example.other")),
              "reject conflicting mapping");
        check(StandaloneDiagnosticRegistry::lookup(root, id) == QLatin1String("org.example.app") &&
              StandaloneDiagnosticRegistry::lookup(root, second).isEmpty(), "lookup exact exported ID");
        const QString armId = QStringLiteral("ru.androidcompatibilitylayer.Android2123456789abcdef01234567");
        const QString armRpm = directory.path() + QLatin1Char('/') + armId + QStringLiteral("-1.0.0-1.armv7hl.rpm");
        QFile armFixture(armRpm);
        check(armFixture.open(QIODevice::WriteOnly), "create ARMv7 exported RPM fixture");
        armFixture.close();
        check(StandaloneDiagnosticRegistry::save(root, armRpm, QStringLiteral("org.example.arm")) &&
              StandaloneDiagnosticRegistry::lookup(root, armId) == QLatin1String("org.example.arm"),
              "register ARMv7 exported mapping");
        check(StandaloneDiagnosticRegistry::appIdFromExecutable(QStringLiteral("/usr/bin/") + id) == id &&
              StandaloneDiagnosticRegistry::appIdFromExecutable(
                  QStringLiteral("/opt/app/") + id + QStringLiteral("/1.0/bin/") + id) == id &&
              StandaloneDiagnosticRegistry::appIdFromExecutable(
                  QStringLiteral("/opt/app/") + id + QStringLiteral("/1.0/bin/") + second).isEmpty(),
              "validate identity executable path");
        IdentityFixture identity;
        identity.executable = QStringLiteral("/usr/bin/") + id;
        check(bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity), "register diagnostic identity");
        check(bus.registerService(QStringLiteral("ru.omp.am")), "register diagnostic identity service");
        QProcess process;
        startServer(process, eventFile);
        QDBusMessage resolve = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"), QStringLiteral("ResolveCaller"));
        QDBusPendingReply<QStringList> resolved(waitFor(bus.asyncCall(resolve)));
        check(resolved.isError() && resolved.error().type() == QDBusError::AccessDenied,
              "background manager refuses registry-only identity mapping");
        identity.executable = QStringLiteral("/usr/bin/") + second;
        QDBusPendingReply<QStringList> unknown(waitFor(bus.asyncCall(resolve)));
        check(unknown.isError() && unknown.error().type() == QDBusError::AccessDenied,
              "background manager rejects unknown standalone");
        identity.executable = QStringLiteral("/usr/bin/") + id;
        QDBusMessage ping = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"), QStringLiteral("DiagnosticPing"));
        QDBusMessage reply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
        check(reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1 &&
              reply.arguments().first().toString() == QLatin1String("synthetic-ok") &&
              identity.sender == bus.baseService(), "registered actual sender gets synthetic response");
        QProcess client;
        QEventLoop clientLoop;
        QTimer clientTimer;
        clientTimer.setSingleShot(true);
        QObject::connect(&client, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                         &clientLoop, &QEventLoop::quit);
        QObject::connect(&clientTimer, &QTimer::timeout, &clientLoop, &QEventLoop::quit);
        client.start(app.applicationFilePath(), QStringList() << QStringLiteral("--diagnostic-client"));
        check(client.waitForStarted(5000), "start separate diagnostic client");
        clientTimer.start(20000);
        clientLoop.exec();
        check(client.state() == QProcess::NotRunning && client.exitCode() == 0,
              "separate diagnostic client completed");
        const QByteArray clientOutput = client.readAllStandardOutput();
        check(clientOutput.contains("owner=present\n") && clientOutput.contains("reply=synthetic-ok\n") &&
              identity.sender.startsWith(QLatin1Char(':')) && identity.sender != bus.baseService(),
              "separate client gets response using its own sender");
        identity.executable = QStringLiteral("/usr/bin/") + second;
        reply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
        check(reply.type() == QDBusMessage::ErrorMessage &&
              reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.AccessDenied"),
              "unknown sender is refused");
        identity.executable = QStringLiteral("/usr/bin/") + id;
        identity.key.clear();
        reply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
        check(reply.type() == QDBusMessage::ErrorMessage &&
              reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.AccessDenied"),
              "unverified identity is refused");
        ping << id;
        reply = QDBusPendingCallWatcher(waitFor(bus.asyncCall(ping))).reply();
        check(reply.type() == QDBusMessage::ErrorMessage, "identity arguments are rejected");
#else
        check(reply.type() == QDBusMessage::ErrorMessage &&
              reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod"),
              "diagnostic method unavailable by default");
#endif
        check(read(eventFile).isEmpty(), "diagnostic did not open manager window");
        process.terminate();
        check(process.waitForFinished(5000), "stop diagnostic manager");
        QProcess restarted;
        startServer(restarted, eventFile);
        QDBusPendingReply<QStringList> afterRestart(waitFor(bus.asyncCall(resolve)));
        check(afterRestart.isError() && afterRestart.error().type() == QDBusError::AccessDenied,
              "manager continues refusing registry-only identity across restart");
        restarted.terminate();
        check(restarted.waitForFinished(5000), "stop restarted manager");
        bus.unregisterService(QStringLiteral("ru.omp.am"));
        bus.unregisterObject(QStringLiteral("/ru/omp/am"));
    }

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
        QDBusMessage start = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/omp/RuntimeManager/IntentHandler1"),
            QStringLiteral("ru.omp.RuntimeManager.IntentHandler1"), QStringLiteral("HandleIntent"));
        start << QStringLiteral("Start") << QVariantMap();
        check(QDBusPendingCallWatcher(waitFor(bus.asyncCall(start))).reply().type() == QDBusMessage::ReplyMessage,
              "RuntimeManager Start intent is answered");
        waitEvents(eventFile, "GUI\n");
        check(read(eventFile).count("GUI\n") == 1, "Start intent opens the background manager window once");
        process.terminate();
        check(process.waitForFinished(5000), "stop started background manager");
        QFile::remove(eventFile);
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
