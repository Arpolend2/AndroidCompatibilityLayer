#include "../aurora/shared_microg_ipc.h"
#include "../aurora/shared_microg_client.h"
#include "../aurora/standalone_diagnostic_registry.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
const char service[] = "test.megler.binder";
const char path[] = "/test/megler/binder";
const char iface[] = "test.megler.binder";
const char managerService[] = "ru.androidcompatibilitylayer.appmanager";
const char standaloneId[] = "ru.androidcompatibilitylayer.Android0123456789abcdef01234567";

class ProviderFixture : public SharedMicrogProviderObject
{
public:
    using SharedMicrogProviderObject::SharedMicrogProviderObject;
    bool legacy = false;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (legacy && message.member() == QLatin1String("OpenClientChannelWithMetadata"))
            return false;
        return SharedMicrogProviderObject::handleMessage(message, connection);
    }
};

class IdentityFixture : public QDBusVirtualObject
{
public:
    QString sender;
    QString managerSender;
    bool untrustedManager = false;
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("ru.omp.am.Identify") ||
            message.member() != QLatin1String("IdentifyByBusName")) return false;
        if (message.arguments().size() != 1 || message.arguments().first().type() != QVariant::String) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad arguments")));
            return true;
        }
        const QString requested = message.arguments().first().toString();
        if (requested == managerSender) {
            connection.send(message.createReply(QVariantList() <<
                (untrustedManager ? QStringLiteral("/usr/bin/dbus-proxy") :
                                    QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.appmanager")) <<
                QStringLiteral("test-key")));
            return true;
        }
        sender = requested;
        connection.send(message.createReply(QVariantList() <<
            (QStringLiteral("/usr/bin/") + QString::fromLatin1(standaloneId)) << QStringLiteral("test-key")));
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

void inheritStream(int fd)
{
    if (fd != 3 && ::dup2(fd, 3) < 0) _exit(125);
    if (fd != 3) ::close(fd);
    const int flags = ::fcntl(3, F_GETFD);
    if (flags < 0 || ::fcntl(3, F_SETFD, flags & ~FD_CLOEXEC) < 0) _exit(125);
}

void execJvm(int fd, const char *role)
{
    inheritStream(fd);
    const char *java = std::getenv("JAVA");
    const char *classpath = std::getenv("CLASSPATH");
    if (!java || !classpath) _exit(125);
    ::execl(java, java, "-cp", classpath, "BinderFdBridgeTest", role, static_cast<char *>(nullptr));
    _exit(125);
}

void execStreamCheck(int fd, bool cancelled = false)
{
    inheritStream(fd);
    char value = 0, extra = 0;
    if (cancelled) {
        if (::read(3, &extra, 1) != 0) _exit(125);
        _exit(0);
    }
    if (::read(3, &value, 1) != 1 || value != 'x' || ::read(3, &extra, 1) != 0) _exit(125);
    _exit(0);
}

void execGuest(int fd, bool server)
{
    inheritStream(fd);
    const char *runner = std::getenv("GUEST_RUNNER");
    const bool headless = server && std::getenv("GUEST_HEADLESS_SERVER");
    const char *apk = std::getenv(headless ? "GUEST_SERVER_APK" : "GUEST_APK");
    const char *data = std::getenv(server ? "GUEST_SERVER_DATA" : "GUEST_DATA");
    const char *root = std::getenv("GUEST_ANDROID_ROOT");
    const char *pictures = std::getenv(server ? "GUEST_SERVER_PICTURES" : "GUEST_PICTURES");
    if (!runner || !apk || !data || !root || !pictures ||
        ::setenv("MEGLER_TEST_BINDER_HOST_FD", "3", 1) != 0 ||
        ::setenv("MEGLER_TEST_BINDER_ROLE", server ? "server" : "client", 1) != 0) _exit(125);
    if (server && std::getenv("MEGLER_TEST_BINDER_BROKER")) {
        const char *gms = std::getenv("GUEST_GMS_APK");
        if (!gms || ::setenv("MG_JAVA_GMS_APK", gms, 1) != 0) _exit(125);
    }
    const char *wait = std::getenv("MEGLER_TEST_BINDER_BROKER") &&
                       std::strcmp(std::getenv("MEGLER_TEST_BINDER_BROKER"), "real") == 0
                       ? "wait:60000" : "wait:15000";
    const char *mode = headless && std::getenv("GUEST_HEADLESS_REAL_SERVER") ? "headless-real-server" :
                       headless && std::getenv("GUEST_HEADLESS_ACTIVITY") ? "headless-activity" :
                       headless ? "headless" : "app";
    ::execl(runner, runner, "--java", mode, apk, data, root, pictures, "1",
            "800x480@160", wait, static_cast<char *>(nullptr));
    _exit(125);
}

class Fixture : public QDBusVirtualObject
{
public:
    pid_t server = -1;
    bool drop = false;
    bool guestServer = false;
    bool clientDisconnected = false;

    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"test.megler.binder\">"
                              "<method name=\"Open\"><arg type=\"h\" direction=\"out\"/>"
                              "</method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String(iface) || message.member() != QLatin1String("Open"))
            return false;
        if (!message.arguments().isEmpty() || server > 0) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad request")));
            return true;
        }
        QDBusUnixFileDescriptor local, peer;
        QString error;
        if (!createSharedMicrogStreamPair(&local, &peer, &error)) {
            connection.send(message.createErrorReply(QDBusError::Failed, error));
            return true;
        }
        const int fd = takeSharedMicrogStream(&local, &error);
        if (fd < 0) {
            connection.send(message.createErrorReply(QDBusError::Failed, error));
            return true;
        }
        server = ::fork();
        if (server == 0) {
            if (guestServer) execGuest(fd, true);
            execJvm(fd, drop ? "server-drop" : "server");
        }
        ::close(fd);
        if (server < 0) {
            connection.send(message.createErrorReply(QDBusError::Failed, QStringLiteral("fork failed")));
            return true;
        }
        if (!connection.send(message.createReply(QVariantList() << QVariant::fromValue(peer))))
            qFatal("Could not send descriptor");
        return true;
    }
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QDBusConnection bus = QDBusConnection::sessionBus();
    check(bus.isConnected(), "session bus");
    const bool guestClient = app.arguments().contains(QStringLiteral("--guest-client"));
    const bool managerGuestClient = app.arguments().contains(QStringLiteral("--manager-guest-client"));
    const bool managerJvmClient = app.arguments().contains(QStringLiteral("--manager-jvm-client"));
    const bool managerSmokeClient = app.arguments().contains(QStringLiteral("--manager-smoke-client"));
    const bool managerCancelClient = app.arguments().contains(QStringLiteral("--manager-cancel-client"));
    const bool managerAuthClient = app.arguments().contains(QStringLiteral("--manager-auth-client"));
    const bool managerMetadataClient = app.arguments().contains(QStringLiteral("--manager-metadata-client"));
    const bool managerLegacyClient = app.arguments().contains(QStringLiteral("--manager-legacy-client"));
    const bool managerUntrustedClient = app.arguments().contains(QStringLiteral("--manager-untrusted-client"));
    const bool managerWrongPackageClient = app.arguments().contains(QStringLiteral("--manager-wrongpackage-client"));
    const bool managerErrorClient = app.arguments().contains(QStringLiteral("--manager-error-client"));
    const bool dropClient = app.arguments().contains(QStringLiteral("--client-drop"));
    if (managerErrorClient) {
        SharedMicrogClientChannel channel;
        QString error;
        check(!channel.open(&error) && !error.isEmpty(), "manager channel reports unavailable service");
        return 0;
    }
    if (managerSmokeClient) {
        SharedMicrogClientChannel channel;
        QString error;
        check(channel.open(&error), "open manager channel for smoke test");
        const int fd = channel.takeStream(&error);
        check(fd >= 0 && ::write(fd, "x", 1) == 1, "write manager channel payload");
        check(channel.takeStream(&error) < 0 && !error.isEmpty(), "reject repeated stream transfer");
        ::close(fd);
        channel.close();
        return 0;
    }
    if (managerCancelClient) {
        SharedMicrogClientChannel channel;
        QString error;
        QTimer::singleShot(0, &app, [&channel]() {
            QString retryError;
            check(!channel.open(&retryError) && !retryError.isEmpty(), "reject reentrant channel open");
            channel.close();
        });
        check(!channel.open(&error) && error.contains(QStringLiteral("cancelled")),
              "cancel pending manager channel open");
        check(channel.takeStream(&error) < 0, "cancelled channel cannot export a late descriptor");
        check(!channel.open(&error), "cancelled channel cannot reopen");
        channel.close();
        return 0;
    }
    if (managerAuthClient || managerMetadataClient || managerLegacyClient ||
        managerUntrustedClient || managerWrongPackageClient) {
        SharedMicrogClientChannel channel;
        QString error;
        const QString package = managerWrongPackageClient ? QStringLiteral("example.wrong") :
                                                          QStringLiteral("megler.test.binderfd");
        const QByteArray metadata("{\"package\":\"megler.test.binderfd\",\"activities\":{"
                                  "\"package\":\"megler.test.binderfd\",\"certificates\":[\"3000\"]}}");
        if (managerMetadataClient) {
            SharedMicrogClientChannel invalid;
            check(!invalid.openClientWithMetadata(package, QByteArray(1024 * 1024 + 1, 'x'), &error) &&
                  !error.isEmpty(), "reject oversized client metadata");
            const QVariantList invalidArguments = QVariantList()
                << QVariant(QStringLiteral("/private/base.apk")) << QVariant(QByteArray())
                << QVariant(QByteArray(1024 * 1024 + 1, 'x'));
            for (const QVariant &argument : invalidArguments) {
                QDBusMessage request = QDBusMessage::createMethodCall(
                    QString::fromLatin1(managerService),
                    QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
                    QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
                    QStringLiteral("OpenClientChannelWithMetadata"));
                request << argument;
                QDBusPendingCallWatcher watcher(bus.asyncCall(request, 5000));
                if (!watcher.isFinished()) {
                    QEventLoop loop;
                    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
                    loop.exec();
                }
                QDBusPendingReply<> reply(watcher);
                check(reply.isError() && reply.error().type() == QDBusError::InvalidArgs,
                      "manager rejects invalid metadata type, empty payload and oversized payload");
            }
        }
        const bool opened = managerMetadataClient || managerLegacyClient
            ? channel.openClientWithMetadata(package, metadata, &error)
            : channel.openClient(package, &error);
        if (!managerAuthClient && !managerMetadataClient && !managerLegacyClient) {
            check(!opened && !error.isEmpty(), "reject untrusted manager or mismatched package");
            check(channel.takeStream(&error) < 0, "rejected manager bootstrap cannot export stream");
            return 0;
        }
        if (!opened) std::fprintf(stderr, "Manager open error: %s\n", qPrintable(error));
        check(opened && channel.uid() == 16464 && channel.pid() >= 50000 &&
              channel.package() == package, "verified atomic manager client identity");
        const int fd = channel.takeStream(&error);
        check(fd >= 0 && ::write(fd, "x", 1) == 1, "verified client can use manager stream");
        ::close(fd);
        channel.close();
        return 0;
    }
    if (guestClient || managerGuestClient || managerJvmClient ||
        dropClient || app.arguments().contains(QStringLiteral("--client"))) {
        SharedMicrogClientChannel managerChannel;
        QString error;
        int fd = -1;
        if (managerGuestClient || managerJvmClient) {
            check(managerChannel.open(&error), "open manager channel");
            fd = managerChannel.takeStream(&error);
            check(fd >= 0, "validate manager channel");
        } else {
            QDBusMessage call = QDBusMessage::createMethodCall(
                QString::fromLatin1(service), QString::fromLatin1(path),
                QString::fromLatin1(iface), QStringLiteral("Open"));
            QDBusPendingCallWatcher watcher(bus.asyncCall(call, 5000));
            if (!watcher.isFinished()) {
                QEventLoop loop;
                QTimer::singleShot(6000, &loop, &QEventLoop::quit);
                QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
                loop.exec();
            }
            check(watcher.isFinished(), "descriptor request timeout");
            QDBusPendingReply<QDBusUnixFileDescriptor> reply(watcher);
            check(!reply.isError(), "descriptor request");
            QDBusUnixFileDescriptor descriptor = reply.value();
            fd = takeSharedMicrogStream(&descriptor, &error);
            check(fd >= 0, "validate descriptor");
        }
        if (managerGuestClient || managerJvmClient) {
            const pid_t guest = ::fork();
            if (guest == 0) {
                if (managerGuestClient) execGuest(fd, false);
                execJvm(fd, "client");
            }
            ::close(fd);
            check(guest > 0, "fork guest client");
            int status = 0;
            check(::waitpid(guest, &status, 0) == guest && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "manager channel client exited successfully");
            return 0;
        }
        if (guestClient) execGuest(fd, false);
        execJvm(fd, dropClient ? "client-drop" : "client");
    }

    Fixture fixture;
    const bool managerGuests = app.arguments().contains(QStringLiteral("--manager-guests"));
    const bool managerJvm = app.arguments().contains(QStringLiteral("--manager-jvm"));
    const bool managerSmoke = app.arguments().contains(QStringLiteral("--manager-smoke"));
    const bool managerCancel = app.arguments().contains(QStringLiteral("--manager-cancel"));
    const bool managerAuth = app.arguments().contains(QStringLiteral("--manager-auth"));
    const bool managerMetadata = app.arguments().contains(QStringLiteral("--manager-metadata"));
    const bool managerLegacy = app.arguments().contains(QStringLiteral("--manager-legacy"));
    const bool managerUntrusted = app.arguments().contains(QStringLiteral("--manager-untrusted"));
    const bool managerWrongPackage = app.arguments().contains(QStringLiteral("--manager-wrongpackage"));
    const bool managerRejected = managerUntrusted || managerWrongPackage;
    const bool managerFixture = managerGuests || managerJvm || managerSmoke || managerCancel ||
                                managerAuth || managerMetadata || managerLegacy || managerRejected;
    fixture.drop = app.arguments().contains(QStringLiteral("--drop"));
    fixture.guestServer = managerGuests || app.arguments().contains(QStringLiteral("--two-guests"));
    QTemporaryDir identityDirectory;
    IdentityFixture identity;
    identity.managerSender = bus.baseService();
    identity.untrustedManager = managerUntrusted;
    QDBusConnection identityBus = QDBusConnection::connectToBus(
        QDBusConnection::SessionBus, QStringLiteral("manager-binder-identity"));
    QDBusServiceWatcher clientWatcher;
    clientWatcher.setConnection(bus);
    clientWatcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(&clientWatcher, &QDBusServiceWatcher::serviceUnregistered, &app,
                     [&fixture](const QString &) { fixture.clientDisconnected = true; });
    ProviderFixture provider(bus, nullptr, {}, identityDirectory.path(),
        [&fixture, &identity, &clientWatcher, managerGuests, managerSmoke, managerCancel, managerAuth, managerMetadata, managerLegacy, managerRejected](const QString &sender, const QString &package, const QByteArray &metadata, int uid, int pid, quint64,
                              const QDBusUnixFileDescriptor &server, QString *error) {
            if (fixture.server > 0 || sender != identity.sender ||
                package != QLatin1String("megler.test.binderfd") || uid != 16464 || pid < 50000) {
                *error = QStringLiteral("Unexpected manager-assigned identity");
                return false;
            }
            if (managerMetadata)
                check(metadata == QByteArray("{\"package\":\"megler.test.binderfd\",\"activities\":{"
                                             "\"package\":\"megler.test.binderfd\",\"certificates\":[\"3000\"]}}"),
                      "manager receives client certificate without reopening its private APK path");
            if (managerLegacy)
                check(metadata.isEmpty(), "legacy server opens a compatible channel without APK metadata");
            clientWatcher.addWatchedService(sender);
            const int fd = ::fcntl(server.fileDescriptor(), F_DUPFD_CLOEXEC, 3);
            if (fd < 0) {
                *error = QStringLiteral("Cannot duplicate server stream");
                return false;
            }
            fixture.server = ::fork();
            if (fixture.server == 0) {
                const QByteArray uidText = QByteArray::number(uid);
                const QByteArray pidText = QByteArray::number(pid);
                if (::setenv("MEGLER_TEST_BINDER_CALLER_UID", uidText.constData(), 1) != 0 ||
                    ::setenv("MEGLER_TEST_BINDER_CALLER_PID", pidText.constData(), 1) != 0) _exit(125);
                if (managerGuests) execGuest(fd, true);
                if (managerSmoke || managerCancel || managerAuth || managerMetadata || managerLegacy || managerRejected)
                    execStreamCheck(fd, managerCancel || managerRejected);
                execJvm(fd, "server");
            }
            ::close(fd);
            if (fixture.server < 0) {
                *error = QStringLiteral("Cannot start test server");
                return false;
            }
            std::printf("ManagerBoundBinder: UID %d PID %d\n", uid, pid);
            std::fflush(stdout);
            return true;
        },
        [&identityDirectory](const QString &executable, const QString &identityKey, QString *error) {
            if (identityKey.isEmpty()) {
                if (error) *error = QStringLiteral("Missing fixture identity key.");
                return QString();
            }
            const QString id = StandaloneDiagnosticRegistry::appIdFromExecutable(executable);
            return StandaloneDiagnosticRegistry::lookup(identityDirectory.path(), id);
        });
    provider.legacy = managerLegacy;
    if (managerFixture) {
        check(identityDirectory.isValid() && identityBus.isConnected(), "create manager identity fixture");
        app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
        app.setApplicationName(QStringLiteral("appmanager"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, identityDirectory.path());
        QSettings settings;
        settings.setValue(QStringLiteral("googleServices/provider"), QStringLiteral("microg"));
        settings.sync();
        check(settings.status() == QSettings::NoError, "select fixture microG provider");
        const QString rpm = identityDirectory.path() + QLatin1Char('/') +
            QString::fromLatin1(standaloneId) + QStringLiteral("-1.0.0-1.x86_64.rpm");
        QFile exported(rpm);
        check(exported.open(QIODevice::WriteOnly), "create standalone fixture");
        exported.close();
        check(StandaloneDiagnosticRegistry::save(identityDirectory.path(), rpm,
                                                  QStringLiteral("megler.test.binderfd")),
              "register fixture standalone");
        QFile uids(identityDirectory.path() + QStringLiteral("/shared-microg-uids.json"));
        check(uids.open(QIODevice::WriteOnly), "create fixture UID registry");
        const QByteArray registry("{\"version\":1,\"packages\":{\"megler.test.binderfd\":16464}}");
        check(uids.write(registry) == registry.size(), "seed legacy-compatible test UID");
        uids.close();
        check(identityBus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) &&
              identityBus.registerService(QStringLiteral("ru.omp.am")), "register fake Aurora identity");
        QString error;
        if (managerMetadata)
            check(provider.setApkMetadataResolver(
                [](const QString &, const QString &, QString *) -> QByteArray {
                    qFatal("The manager must not reopen an APK from the client's filesystem namespace");
                    return QByteArray();
                }, &error), "install inaccessible-client-filesystem regression guard");
        check(bus.registerService(QString::fromLatin1(managerService)) && provider.start(&error),
              "start manager channel fixture");
    } else {
        check(bus.registerService(QString::fromLatin1(service)), "register fixture service");
        check(bus.registerVirtualObject(QString::fromLatin1(path), &fixture), "register fixture object");
    }
    QProcess client;
    client.setProcessChannelMode(QProcess::ForwardedChannels);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&client, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const bool guest = fixture.guestServer || app.arguments().contains(QStringLiteral("--guest"));
    client.start(app.applicationFilePath(), QStringList() <<
                 (managerGuests ? QStringLiteral("--manager-guest-client") :
                  managerJvm ? QStringLiteral("--manager-jvm-client") :
                  managerSmoke ? QStringLiteral("--manager-smoke-client") :
                  managerCancel ? QStringLiteral("--manager-cancel-client") :
                  managerAuth ? QStringLiteral("--manager-auth-client") :
                  managerMetadata ? QStringLiteral("--manager-metadata-client") :
                  managerLegacy ? QStringLiteral("--manager-legacy-client") :
                  managerUntrusted ? QStringLiteral("--manager-untrusted-client") :
                  managerWrongPackage ? QStringLiteral("--manager-wrongpackage-client") :
                  app.arguments().contains(QStringLiteral("--manager-error")) ? QStringLiteral("--manager-error-client") :
                  guest ? QStringLiteral("--guest-client") :
                  fixture.drop ? QStringLiteral("--client-drop") : QStringLiteral("--client")));
    check(client.waitForStarted(5000), "start client");
    timeout.start(guest ? 90000 : 10000);
    loop.exec();
    if (client.state() != QProcess::NotRunning) {
        client.kill();
        client.waitForFinished(5000);
        if (fixture.server > 0) { ::kill(fixture.server, SIGKILL); ::waitpid(fixture.server, nullptr, 0); }
        check(false, "client timed out");
    }
    const bool clientOk = client.exitStatus() == QProcess::NormalExit && client.exitCode() == 0;
    if (app.arguments().contains(QStringLiteral("--manager-error"))) {
        check(clientOk, "client handles unavailable manager service");
        return 0;
    }
    int status = 0;
    pid_t result = -1;
    QElapsedTimer elapsed;
    elapsed.start();
    while (fixture.server > 0 && elapsed.elapsed() < 5000) {
        app.processEvents();
        result = ::waitpid(fixture.server, &status, WNOHANG);
        if (result != 0) break;
        QThread::msleep(10);
    }
    if (result == 0) {
        ::kill(fixture.server, SIGKILL);
        ::waitpid(fixture.server, &status, 0);
    }
    check(clientOk, "client JVM");
    if (managerFixture)
        check(fixture.clientDisconnected, "manager client D-Bus sender disconnected");
    if (result != fixture.server || fixture.server <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        std::fprintf(stderr, "FAIL: server child result=%ld pid=%ld status=%d\n",
                     static_cast<long>(result), static_cast<long>(fixture.server), status);
    check(result == fixture.server && fixture.server > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "server process exited after disconnect");
    return 0;
}
