#include "../aurora/shared_microg_ipc.h"
#include "../aurora/standalone_diagnostic_registry.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef ACL_TEST_GUEST_FD
#include "../core/src/mg_core.h"
#include "../core/src/bionic/mg_fd.h"
#include "../core/src/bionic/mg_net.h"
#endif

namespace {
const char service[] = "ru.androidcompatibilitylayer.appmanager";
const char path[] = "/ru/androidcompatibilitylayer/appmanager/microg";
const char interface[] = "ru.androidcompatibilitylayer.appmanager.microg";

class IdentityFixture : public QDBusVirtualObject
{
public:
    QStringList seen;
    bool malformed = false;
    int delayMs = 0;
    QString executable = QStringLiteral("/test/client");
    QString key = QStringLiteral("test-key");
    std::function<void()> onIdentify;

    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"ru.omp.am.Identify\">"
            "<method name=\"IdentifyByBusName\"><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
            "</method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("ru.omp.am.Identify") ||
            message.member() != QLatin1String("IdentifyByBusName")) return false;
        if (message.arguments().size() != 1 || message.arguments().first().type() != QVariant::String) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad arguments")));
            return true;
        }
        seen.append(message.arguments().first().toString());
        if (onIdentify) onIdentify();
        const QVariantList result = malformed
            ? QVariantList() << 123 << QStringLiteral("key")
            : QVariantList() << executable << key;
        QTimer::singleShot(delayMs, this, [message, connection, result]() {
            connection.send(message.createReply(result));
        });
        return true;
    }
};

class StreamFixture : public QDBusVirtualObject
{
public:
    int fd = -1;
    QByteArray received;
    QString sender;
    bool disconnected = false;

    ~StreamFixture() override { if (fd >= 0) ::close(fd); }

    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name=\"test.microg.stream\">"
            "<method name=\"Open\"><arg type=\"h\" direction=\"out\"/></method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("test.microg.stream") ||
            message.member() != QLatin1String("Open")) return false;
        if (!message.arguments().isEmpty() || fd >= 0) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("bad request")));
            return true;
        }
        QDBusUnixFileDescriptor local, peer;
        QString error;
        if (!createSharedMicrogStreamPair(&local, &peer, &error) ||
            (fd = takeSharedMicrogStream(&local, &error)) < 0) {
            connection.send(message.createErrorReply(QDBusError::Failed, error));
            return true;
        }
        sender = message.service();
        QSocketNotifier *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this, notifier](int) {
            char bytes[16];
            const ssize_t count = ::read(fd, bytes, sizeof(bytes));
            if (count == 0) { disconnected = true; notifier->setEnabled(false); return; }
            if (count < 0) { notifier->setEnabled(false); return; }
            received.append(bytes, static_cast<int>(count));
            if (received == "ping") {
                if (::write(fd, "pong", 4) != 4) notifier->setEnabled(false);
            }
        });
        if (!connection.send(message.createReply(QVariantList() << QVariant::fromValue(peer))))
            qFatal("Could not send test descriptor");
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

QDBusPendingCall waitFor(const QDBusPendingCall &call)
{
    QDBusPendingCallWatcher watcher(call);
    if (!watcher.isFinished()) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(8000);
        loop.exec();
    }
    check(watcher.isFinished(), "D-Bus request timed out");
    return watcher;
}

QDBusPendingReply<QString> request(const QDBusConnection &bus)
{
    return QDBusPendingReply<QString>(waitFor(requestSharedMicrogProvider(bus, false)));
}

QDBusPendingCall inspect(const QDBusConnection &bus, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("InspectCaller"));
    message.setArguments(arguments);
    return bus.asyncCall(message, 6000);
}

QDBusPendingCall resolve(const QDBusConnection &bus, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("ResolveCaller"));
    message.setArguments(arguments);
    return bus.asyncCall(message, 6000);
}

QDBusPendingCall openChannel(const QDBusConnection &bus, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("OpenChannel"));
    message.setArguments(arguments);
    return bus.asyncCall(message, 6000);
}

void setProvider(const QString &value)
{
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), value);
    settings.sync();
    check(settings.status() == QSettings::NoError, "write isolated settings");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--departing-client")) ||
        app.arguments().contains(QStringLiteral("--departing-channel-client"))) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        check(bus.isConnected(), "connect departing client");
        if (app.arguments().contains(QStringLiteral("--departing-channel-client"))) openChannel(bus);
        else resolve(bus);
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&input, &QSocketNotifier::activated, &app, [&app](int fd) {
            char byte;
            if (::read(fd, &byte, 1) == 1) app.quit();
        });
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (app.arguments().contains(QStringLiteral("--stream-client"))) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        QDBusMessage call = QDBusMessage::createMethodCall(
            QString::fromLatin1(service), QStringLiteral("/test/microg/stream"),
            QStringLiteral("test.microg.stream"), QStringLiteral("Open"));
        QDBusPendingReply<QDBusUnixFileDescriptor> reply(waitFor(bus.asyncCall(call, 6000)));
        check(!reply.isError(), "receive stream descriptor through D-Bus");
        QDBusUnixFileDescriptor descriptor = reply.value();
        QString error;
        const int fd = takeSharedMicrogStream(&descriptor, &error);
        check(fd >= 0 && (::fcntl(fd, F_GETFD) & FD_CLOEXEC), "validate received stream and close-on-exec");
#ifdef ACL_TEST_GUEST_FD
        check(mg_core_init(), "initialize guest runtime");
        const int guestFd = mg_net_import_unix_stream(fd);
        check(guestFd >= 0, "import transferred stream into guest descriptor table");
        ::close(fd);
        check(mg_fd_write(guestFd, "ping", 4) == 4, "write through guest descriptor");
        char answer[4];
        size_t used = 0;
        while (used < sizeof(answer)) {
            const i32 n = mg_fd_read(guestFd, answer + used, sizeof(answer) - used);
            check(n > 0, "read through guest descriptor");
            used += static_cast<size_t>(n);
        }
        check(QByteArray(answer, 4) == "pong", "reply through guest descriptor");
        check(mg_fd_close(guestFd) == 0, "close guest descriptor");
#else
        check(::write(fd, "ping", 4) == 4, "write through transferred stream");
        char answer[4];
        size_t used = 0;
        while (used < sizeof(answer)) {
            const ssize_t n = ::read(fd, answer + used, sizeof(answer) - used);
            check(n > 0, "read through transferred stream");
            used += static_cast<size_t>(n);
        }
        check(QByteArray(answer, 4) == "pong", "reply through transferred stream");
        ::close(fd);
#endif
        return 0;
    }
    app.setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    app.setApplicationName(QStringLiteral("appmanager"));
    QTemporaryDir temporary;
    check(temporary.isValid(), "create isolated settings directory");
    const QString identityDirectory = QDir(temporary.path()).filePath(QStringLiteral("identity"));
    check(QDir().mkpath(identityDirectory), "create identity directory");
    const QString appId = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    QFile exported(QDir(identityDirectory).filePath(appId + QStringLiteral("-1.0.0-1.x86_64.rpm")));
    check(exported.open(QIODevice::WriteOnly), "create exported RPM fixture");
    exported.close();
    check(StandaloneDiagnosticRegistry::save(identityDirectory, exported.fileName(), QStringLiteral("org.example.app")),
          "record exported standalone package");
    const SharedMicrogProviderObject::PackageResolver packageResolver =
        [identityDirectory](const QString &executable, const QString &identityKey, QString *resolverError) {
            if (identityKey.isEmpty()) {
                if (resolverError) *resolverError = QStringLiteral("Missing fixture identity key.");
                return QString();
            }
            return StandaloneDiagnosticRegistry::lookup(
                identityDirectory, StandaloneDiagnosticRegistry::appIdFromExecutable(executable));
        };
    QString provider, error;
    int firstUid = -1, secondUid = -1, repeatedUid = -1;
    check(sharedMicrogUidForPackage(temporary.path(), QStringLiteral("com.example.Aa"), &firstUid, &error) &&
          sharedMicrogUidForPackage(temporary.path(), QStringLiteral("com.example.BB"), &secondUid, &error) &&
          firstUid != secondUid, "colliding package hashes receive distinct UIDs");
    check(sharedMicrogUidForPackage(temporary.path(), QStringLiteral("com.example.Aa"), &repeatedUid, &error) &&
          repeatedUid == firstUid, "UID remains stable after reopening registry");
    check(!sharedMicrogUidForPackage(temporary.path(), QStringLiteral("invalid/package"), &repeatedUid, &error) &&
          repeatedUid == -1 && !error.isEmpty(), "reject invalid package");
    QFile registry(QDir(temporary.path()).filePath(QStringLiteral("shared-microg-uids.json")));
    check(registry.open(QIODevice::WriteOnly | QIODevice::Truncate), "open registry corruption fixture");
    const QByteArray corrupt("{\"version\":1,\"packages\":{\"com.example.one\":11000,\"com.example.two\":11000}}");
    check(registry.write(corrupt) == corrupt.size(), "write duplicate UID fixture");
    registry.close();
    check(!sharedMicrogUidForPackage(temporary.path(), QStringLiteral("com.example.three"), &repeatedUid, &error) &&
          repeatedUid == -1 && !error.isEmpty(), "reject duplicate UID registry without rewriting");
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
    QDBusConnection bus = QDBusConnection::sessionBus();
    check(bus.isConnected(), "connect to session bus");

    check(!readSharedMicrogProvider(request(bus), &provider, &error) && !error.isEmpty(),
          "unavailable manager reports an error");
    const QString marker = QString::fromLocal8Bit(qgetenv("ACL_AUTOSTART_MARKER"));
    check(!marker.isEmpty() && !QFile::exists(marker), "status request does not activate service");
    check(!bus.interface()->isServiceRegistered(QString::fromLatin1(service)).value(),
          "status request does not start the manager");
    {
        SharedMicrogProviderObject notOwner(bus);
        check(!notOwner.start(&error) && !error.isEmpty(), "object requires manager name owner");
    }

    check(bus.registerService(QString::fromLatin1(service)), "own manager name for test");
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                         QStringLiteral("microg-test-other"));
    check(other.isConnected(), "connect second bus client");
    {
        SharedMicrogProviderObject notOwner(other);
        check(!notOwner.start(&error) && !error.isEmpty(), "other connection cannot register service object");
    }
    {
        QString channelSender, channelPackage;
        int channelUid = -1, channelPid = -1, serverFd = -1, channelCalls = 0;
        quint64 channelId = 0;
        bool refuseChannel = false, releaseDuringHandoff = false;
        SharedMicrogProviderObject *providerObject = nullptr;
        SharedMicrogProviderObject object(bus, nullptr, {}, identityDirectory,
            [&](const QString &sender, const QString &package, int uid, int pid, quint64 id,
                const QDBusUnixFileDescriptor &server, QString *channelError) {
                ++channelCalls;
                channelSender = sender;
                channelPackage = package;
                channelUid = uid;
                channelPid = pid;
                channelId = id;
                if (refuseChannel) {
                    *channelError = QStringLiteral("server unavailable");
                    return false;
                }
                if (releaseDuringHandoff) {
                    providerObject->releaseChannel(sender, id);
                    return true;
                }
                serverFd = ::fcntl(server.fileDescriptor(), F_DUPFD_CLOEXEC, 0);
                if (serverFd < 0) {
                    *channelError = QStringLiteral("cannot duplicate server descriptor");
                    return false;
                }
                return true;
            }, packageResolver);
        providerObject = &object;
        check(object.start(&error), "register provider object");
        check(!object.start(&error) && !error.isEmpty(), "reject duplicate object registration");
        {
            QDBusUnixFileDescriptor local, peer;
            check(!createSharedMicrogStreamPair(&local, &local, &error) && !error.isEmpty(),
                  "reject aliased pair outputs");
            const int nullFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            check(nullFd >= 0, "open non-socket descriptor");
            local.giveFileDescriptor(nullFd);
            check(takeSharedMicrogStream(&local, &error) < 0 && local.isValid(),
                  "reject non-socket descriptor without consuming it");
            check(!createSharedMicrogStreamPair(&local, &peer, &error), "reject non-empty output");
        }
        {
            QDBusUnixFileDescriptor unconnected;
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            check(fd >= 0, "create unconnected socket");
            unconnected.giveFileDescriptor(fd);
            check(takeSharedMicrogStream(&unconnected, &error) < 0 && unconnected.isValid(),
                  "reject unconnected stream");
        }
        {
            StreamFixture stream;
            check(bus.registerVirtualObject(QStringLiteral("/test/microg/stream"), &stream),
                  "register isolated stream fixture");
            QProcess client;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&client, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                             &loop, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            client.start(app.applicationFilePath(), QStringList() << QStringLiteral("--stream-client"));
            check(client.waitForStarted(5000), "start separate stream client");
            timeout.start(8000);
            loop.exec();
            if (client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0)
                std::fprintf(stderr, "Stream client stderr: %s\n", client.readAllStandardError().constData());
            check(client.state() == QProcess::NotRunning && client.exitStatus() == QProcess::NormalExit &&
                  client.exitCode() == 0, "separate client used transferred stream");
            check(stream.received == "ping" && stream.sender.startsWith(QLatin1Char(':')),
                  "stream request came through D-Bus");
            QEventLoop eofLoop;
            QTimer::singleShot(100, &eofLoop, &QEventLoop::quit);
            eofLoop.exec();
            check(stream.disconnected, "peer close reaches stream owner");
            bus.unregisterObject(QStringLiteral("/test/microg/stream"));
        }
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("none"), "default provider is none");
        setProvider(QStringLiteral("microg"));
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("microg"), "read microg from manager settings");
        setProvider(QStringLiteral("none"));
        check(readSharedMicrogProvider(request(other), &provider, &error) &&
              provider == QLatin1String("none"), "read updated provider");
        setProvider(QStringLiteral("invalid"));
        check(!readSharedMicrogProvider(request(other), &provider, &error) && !error.isEmpty(),
              "unknown provider returns an error");

        QDBusMessage invalid = QDBusMessage::createMethodCall(
            QString::fromLatin1(service), QString::fromLatin1(path),
            QString::fromLatin1(interface), QStringLiteral("GetProvider"));
        invalid << QStringLiteral("unexpected");
        QDBusPendingReply<QString> invalidReply(waitFor(other.asyncCall(invalid)));
        check(invalidReply.isError() &&
              invalidReply.error().type() == QDBusError::InvalidArgs, "reject unexpected arguments");

        QDBusPendingReply<QStringList> failedIdentity(waitFor(inspect(other)));
        check(failedIdentity.isError(), "unavailable identity service returns an error");
        IdentityFixture identity;
        QDBusConnection identityBus = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-identity"));
        QDBusConnection other2 = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-other2"));
        check(identityBus.isConnected() && other2.isConnected(), "connect identity fixture and second client");
        check(identityBus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity),
              "register identity test object");
        check(identityBus.registerService(QStringLiteral("ru.omp.am")), "register identity test service");
        QDBusPendingReply<QStringList> identityReply(waitFor(inspect(other)));
        check(!identityReply.isError() &&
              identityReply.value() == (QStringList() << other.baseService()
                                        << QStringLiteral("/test/client") << QStringLiteral("test-key")) &&
              identity.seen.last() == other.baseService(),
              "identify the real sender of the other connection");
        QDBusPendingReply<QStringList> identityReply2(waitFor(inspect(other2)));
        check(!identityReply2.isError() && identityReply2.value().first() == other2.baseService() &&
              identity.seen.last() == other2.baseService() && other2.baseService() != other.baseService(),
              "identify distinct D-Bus connections independently");
        QDBusPendingReply<QStringList> unknownCaller(waitFor(resolve(other)));
        check(unknownCaller.isError(), "unrecognized executable cannot obtain identity");
        QDBusPendingReply<QDBusUnixFileDescriptor> unknownChannel(waitFor(openChannel(other)));
        check(unknownChannel.isError() && channelCalls == 0,
              "unknown caller cannot open a channel");
        identity.executable = QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.Android"
                                             "ffffffffffffffffffffffff");
        QDBusPendingReply<QStringList> unknownId(waitFor(resolve(other)));
        check(unknownId.isError() && unknownId.error().type() == QDBusError::AccessDenied,
              "unregistered standalone ID is rejected");
        identity.executable = QStringLiteral("/usr/bin/") + appId;
        QDBusPendingReply<QStringList> resolved(waitFor(resolve(other)));
        check(!resolved.isError() && resolved.value().size() == 4 &&
              resolved.value().at(0) == other.baseService() &&
              resolved.value().at(1) == QLatin1String("org.example.app") &&
              resolved.value().at(2).toInt() >= 11000 && resolved.value().at(3).toInt() >= 50000,
              "resolve actual sender to package and allocated identities");
        QDBusPendingReply<QStringList> sameCaller(waitFor(resolve(other)));
        QDBusPendingReply<QStringList> secondCaller(waitFor(resolve(other2)));
        check(!sameCaller.isError() && sameCaller.value() == resolved.value() &&
              !secondCaller.isError() && secondCaller.value().at(2) == resolved.value().at(2) &&
              secondCaller.value().at(3) != resolved.value().at(3),
              "UID is per package and virtual PID is per sender");
        setProvider(QStringLiteral("none"));
        QDBusPendingReply<QDBusUnixFileDescriptor> disabledChannel(waitFor(openChannel(other2)));
        check(disabledChannel.isError() && disabledChannel.error().type() == QDBusError::AccessDenied &&
              channelCalls == 0, "disabled provider cannot open a channel");
        setProvider(QStringLiteral("invalid"));
        QDBusPendingReply<QDBusUnixFileDescriptor> invalidProviderChannel(waitFor(openChannel(other2)));
        check(invalidProviderChannel.isError() && channelCalls == 0,
              "invalid provider setting cannot open a channel");
        setProvider(QStringLiteral("microg"));
        identity.delayMs = 100;
        identity.onIdentify = []() { setProvider(QStringLiteral("none")); };
        QDBusPendingReply<QDBusUnixFileDescriptor> switchedProvider(waitFor(openChannel(other2)));
        check(switchedProvider.isError() && switchedProvider.error().type() == QDBusError::AccessDenied &&
              channelCalls == 0, "provider is checked after asynchronous identification");
        identity.onIdentify = {};
        identity.delayMs = 0;
        setProvider(QStringLiteral("microg"));
        QDBusPendingReply<QDBusUnixFileDescriptor> forgedChannel(
            waitFor(openChannel(other2, QVariantList() << other.baseService())));
        check(forgedChannel.isError() && forgedChannel.error().type() == QDBusError::InvalidArgs &&
              channelCalls == 0, "channel caller cannot supply an identity");
        QDBusMessage noReply = QDBusMessage::createMethodCall(
            QString::fromLatin1(service), QString::fromLatin1(path),
            QString::fromLatin1(interface), QStringLiteral("OpenChannel"));
        const int beforeNoReply = identity.seen.size();
        other2.call(noReply, QDBus::NoBlock);
        {
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
        }
        check(channelCalls == 0 && identity.seen.size() == beforeNoReply,
              "one-way request cannot create an orphaned channel");
        refuseChannel = true;
        QDBusPendingReply<QDBusUnixFileDescriptor> refusedChannel(waitFor(openChannel(other2)));
        check(refusedChannel.isError() && channelCalls == 1, "server rejection fails channel request");
        refuseChannel = false;
        releaseDuringHandoff = true;
        QDBusPendingReply<QDBusUnixFileDescriptor> closedChannel(waitFor(openChannel(other2)));
        check(closedChannel.isError() && channelCalls == 2,
              "backend closing during handoff cannot produce a successful reply");
        releaseDuringHandoff = false;
        QDBusPendingReply<QDBusUnixFileDescriptor> opened(waitFor(openChannel(other2)));
        check(!opened.isError() && channelCalls == 3 && channelSender == other2.baseService() &&
              channelPackage == QLatin1String("org.example.app") &&
              channelUid == secondCaller.value().at(2).toInt() &&
              channelPid == secondCaller.value().at(3).toInt() && channelId != 0 && serverFd >= 0,
              "manager delivers its identified caller and server descriptor to the backend");
        QDBusUnixFileDescriptor received = opened.value();
        int clientFd = takeSharedMicrogStream(&received, &error);
        check(clientFd >= 0, "client receives connected descriptor");
        char byte = 0;
        check(::write(clientFd, "p", 1) == 1 && ::read(serverFd, &byte, 1) == 1 && byte == 'p' &&
              ::write(serverFd, "q", 1) == 1 && ::read(clientFd, &byte, 1) == 1 && byte == 'q',
              "manager-issued channel is duplex");
        QDBusPendingReply<QDBusUnixFileDescriptor> duplicateChannel(waitFor(openChannel(other2)));
        check(duplicateChannel.isError() && duplicateChannel.error().type() == QDBusError::LimitsExceeded &&
              channelCalls == 3, "one channel per caller");
        const quint64 firstChannelId = channelId;
        object.releaseChannel(channelSender, firstChannelId);
        check(::read(clientFd, &byte, 1) == 0, "server release revokes client stream");
        ::close(clientFd);
        ::close(serverFd);
        serverFd = -1;
        QDBusPendingReply<QDBusUnixFileDescriptor> reopened(waitFor(openChannel(other2)));
        check(!reopened.isError() && channelId != firstChannelId && serverFd >= 0,
              "caller can open a fresh channel after release");
        received = reopened.value();
        clientFd = takeSharedMicrogStream(&received, &error);
        check(clientFd >= 0, "receive reopened channel");
        object.releaseChannel(channelSender, firstChannelId);
        check(::write(clientFd, "r", 1) == 1 && ::read(serverFd, &byte, 1) == 1 && byte == 'r',
              "stale release cannot revoke a replacement channel");
        object.refreshProviderSetting();
        check(::write(clientFd, "s", 1) == 1 && ::read(serverFd, &byte, 1) == 1 && byte == 's',
              "unchanged microG setting keeps the channel open");
        const int firstServerFd = serverFd;
        QDBusPendingReply<QDBusUnixFileDescriptor> otherOpened(waitFor(openChannel(other)));
        check(!otherOpened.isError(), "open a second caller channel");
        QDBusUnixFileDescriptor otherReceived = otherOpened.value();
        const int otherClientFd = takeSharedMicrogStream(&otherReceived, &error);
        check(otherClientFd >= 0 && serverFd >= 0, "receive second caller channel");
        setProvider(QStringLiteral("none"));
        object.refreshProviderSetting();
        check(::read(clientFd, &byte, 1) == 0 && ::read(otherClientFd, &byte, 1) == 0,
              "manager setting change revokes all active channels");
        ::close(clientFd);
        ::close(otherClientFd);
        ::close(firstServerFd);
        ::close(serverFd);
        setProvider(QStringLiteral("microg"));
        reopened = QDBusPendingReply<QDBusUnixFileDescriptor>(waitFor(openChannel(other2)));
        check(!reopened.isError(), "open channel after re-enabling microG");
        received = reopened.value();
        clientFd = takeSharedMicrogStream(&received, &error);
        check(clientFd >= 0, "receive channel for external setting test");
        setProvider(QStringLiteral("invalid"));
        {
            QEventLoop loop;
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            loop.exec();
        }
        struct pollfd ready = {clientFd, POLLIN, 0};
        check(::poll(&ready, 1, 2000) == 1, "external setting revocation reaches client");
        check(::read(clientFd, &byte, 1) == 0, "invalid external setting revokes active channel");
        ::close(clientFd);
        ::close(serverFd);
        setProvider(QStringLiteral("microg"));
        reopened = QDBusPendingReply<QDBusUnixFileDescriptor>(waitFor(openChannel(other2)));
        check(!reopened.isError(), "open channel after correcting provider");
        received = reopened.value();
        clientFd = takeSharedMicrogStream(&received, &error);
        check(clientFd >= 0, "receive channel for disconnect test");
        QDBusServiceWatcher *senderWatcher = object.findChild<QDBusServiceWatcher *>();
        check(senderWatcher && senderWatcher->watchedServices().contains(other2.baseService()),
              "resolved caller is watched");
        const QString disconnectedSender = other2.baseService();
        {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(senderWatcher, &QDBusServiceWatcher::serviceUnregistered, &loop,
                             [&loop, disconnectedSender](const QString &name) {
                if (name == disconnectedSender) loop.quit();
            }, packageResolver);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(3000);
            QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-other2"));
            other2 = bus;
            loop.exec();
            check(timeout.isActive(), "disconnect notification arrives");
            QCoreApplication::processEvents();
            check(!senderWatcher->watchedServices().contains(disconnectedSender),
                  "disconnect releases caller watch");
        }
        check(::read(clientFd, &byte, 1) == 0, "D-Bus disconnect revokes retained client stream");
        ::close(clientFd);
        ::close(serverFd);
        QDBusConnection other3 = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-other3"));
        check(other3.isConnected(), "reconnect client");
        QDBusPendingReply<QStringList> reconnected(waitFor(resolve(other3)));
        check(!reconnected.isError() && reconnected.value().at(2) == resolved.value().at(2) &&
              reconnected.value().at(3) != secondCaller.value().at(3),
              "reconnected caller keeps UID but receives a new virtual PID");

        identity.delayMs = 1000;
        const int beforeDeparting = identity.seen.size();
        const int beforeDepartingChannels = channelCalls;
        QProcess departing;
        identity.onIdentify = [&departing]() {
            check(departing.write("x", 1) == 1, "release departing client after identification starts");
        };
        QEventLoop departureLoop;
        QTimer departureTimeout;
        departureTimeout.setSingleShot(true);
        QObject::connect(&departing, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                         &departureLoop, &QEventLoop::quit);
        QObject::connect(&departureTimeout, &QTimer::timeout, &departureLoop, &QEventLoop::quit);
        departing.start(app.applicationFilePath(), QStringList() << QStringLiteral("--departing-channel-client"));
        check(departing.waitForStarted(5000), "start departing client");
        departureTimeout.start(3000);
        departureLoop.exec();
        check(departing.state() == QProcess::NotRunning && departing.exitStatus() == QProcess::NormalExit &&
              departing.exitCode() == 0, "client exits during identification");
        check(identity.seen.size() == beforeDeparting + 1,
              "identification started before client disconnect");
        identity.onIdentify = {};
        const QString departingSender = identity.seen.last();
        {
            QEventLoop loop;
            QTimer::singleShot(1200, &loop, &QEventLoop::quit);
            loop.exec();
        }
        check(!senderWatcher->watchedServices().contains(departingSender) &&
              channelCalls == beforeDepartingChannels,
              "disconnect during identification does not open a channel or leave a watch");
        identity.delayMs = 0;
        QDBusConnection afterDeparture = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-after-departure"));
        QDBusPendingReply<QStringList> afterDeparted(waitFor(resolve(afterDeparture)));
        check(!afterDeparted.isError() &&
              afterDeparted.value().at(3).toInt() == reconnected.value().at(3).toInt() + 1,
              "disconnected caller does not consume a virtual PID");
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-after-departure"));
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-other3"));

        QFile identityRegistry(QDir(identityDirectory).filePath(QStringLiteral("shared-microg-uids.json")));
        check(identityRegistry.open(QIODevice::ReadOnly), "read identity registry fixture");
        const QByteArray savedRegistry = identityRegistry.readAll();
        identityRegistry.close();
        check(identityRegistry.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              identityRegistry.write(corrupt) == corrupt.size(), "corrupt identity registry fixture");
        identityRegistry.close();
        QDBusPendingReply<QStringList> corruptIdentity(waitFor(resolve(other)));
        check(corruptIdentity.isError(), "corrupted UID registry rejects caller");
        QDBusConnection badRegistryClient = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-bad-registry"));
        QDBusPendingReply<QStringList> corruptNewIdentity(waitFor(resolve(badRegistryClient)));
        check(corruptNewIdentity.isError() &&
              !senderWatcher->watchedServices().contains(badRegistryClient.baseService()),
              "registry failure removes a newly installed caller watch");
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-bad-registry"));
        check(identityRegistry.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              identityRegistry.write(savedRegistry) == savedRegistry.size(), "restore identity registry fixture");
        identityRegistry.close();
        QDBusPendingReply<QStringList> restoredIdentity(waitFor(resolve(other)));
        check(!restoredIdentity.isError() && restoredIdentity.value() == resolved.value(),
              "caller identity recovers after registry repair");
        QDBusPendingReply<QStringList> spoofResolve(waitFor(resolve(other, QVariantList() << disconnectedSender)));
        check(spoofResolve.isError() && spoofResolve.error().type() == QDBusError::InvalidArgs,
              "caller cannot select a sender or package");
        identity.key.clear();
        QDBusPendingReply<QStringList> noKey(waitFor(resolve(other)));
        check(noKey.isError(), "missing platform key is rejected");
        identity.key = QStringLiteral("test-key");
        identity.executable = QStringLiteral("/test/client");
        const int beforeSpoof = identity.seen.size();
        QDBusPendingReply<QStringList> spoof(waitFor(inspect(other, QVariantList() << bus.baseService())));
        check(spoof.isError() && spoof.error().type() == QDBusError::InvalidArgs &&
              identity.seen.size() == beforeSpoof, "reject a caller-supplied identity");
        identity.malformed = true;
        QDBusPendingReply<QStringList> malformed(waitFor(inspect(other)));
        check(malformed.isError(), "reject malformed identification response");
        identity.malformed = false;
        identity.delayMs = 300;
        QList<QDBusPendingCall> concurrent;
        for (int i = 0; i < 17; ++i) concurrent.append(inspect(other));
        QDBusPendingReply<QStringList> limited(waitFor(concurrent.last()));
        check(limited.isError() && limited.error().type() == QDBusError::LimitsExceeded,
              "limit concurrent identification requests");
        for (int i = 0; i < 16; ++i) {
            QDBusPendingReply<QStringList> completed(waitFor(concurrent.at(i)));
            check(!completed.isError(), "in-flight identification completes");
        }
        identity.delayMs = 0;
        QDBusPendingReply<QStringList> afterLimit(waitFor(inspect(other)));
        check(!afterLimit.isError(), "identification capacity is released");
        identity.delayMs = 5500;
        QDBusPendingReply<QStringList> timedOut(waitFor(inspect(other)));
        check(timedOut.isError(), "identity service timeout returns an error");
        identity.delayMs = 0;
        QDBusPendingReply<QStringList> afterTimeout(waitFor(inspect(other)));
        check(!afterTimeout.isError(), "identification capacity is released after timeout");
        identityBus.unregisterService(QStringLiteral("ru.omp.am"));
        identityBus.unregisterObject(QStringLiteral("/ru/omp/am"));
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-identity"));

        const QString settingsPath = temporary.path() + QStringLiteral("/bad-settings");
        check(QDir().mkpath(settingsPath + QStringLiteral("/ru.androidcompatibilitylayer")),
              "create invalid settings directory");
        QFile file(settingsPath + QStringLiteral("/ru.androidcompatibilitylayer/appmanager.conf"));
        check(file.open(QIODevice::WriteOnly), "create malformed settings");
        check(file.write("[invalid\nkey=value\n") > 0, "write malformed settings");
        file.close();
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsPath);
        check(!readSharedMicrogProvider(request(other), &provider, &error) && !error.isEmpty(),
              "settings read failure returns an error");
    }
    {
        IdentityFixture identity;
        identity.executable = QStringLiteral("/usr/bin/") + appId;
        QDBusConnection identityBus = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("microg-test-destruction-identity"));
        check(identityBus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) &&
              identityBus.registerService(QStringLiteral("ru.omp.am")), "register destruction identity");
        int serverFd = -1, clientFd = -1;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
        setProvider(QStringLiteral("microg"));
        {
            SharedMicrogProviderObject object(bus, nullptr, {}, identityDirectory,
                [&serverFd](const QString &, const QString &, int, int, quint64,
                            const QDBusUnixFileDescriptor &server, QString *channelError) {
                    serverFd = ::fcntl(server.fileDescriptor(), F_DUPFD_CLOEXEC, 0);
                    if (serverFd >= 0) return true;
                    *channelError = QStringLiteral("cannot duplicate server descriptor");
                    return false;
                }, packageResolver);
            check(object.start(&error), "start provider for destruction test");
            QDBusPendingReply<QDBusUnixFileDescriptor> opened(waitFor(openChannel(other)));
            check(!opened.isError(), "open channel before provider destruction");
            QDBusUnixFileDescriptor client = opened.value();
            clientFd = takeSharedMicrogStream(&client, &error);
            check(clientFd >= 0 && serverFd >= 0, "retain both channel ends");
        }
        char byte = 0;
        check(::read(clientFd, &byte, 1) == 0, "provider destruction revokes active channel");
        ::close(clientFd);
        ::close(serverFd);
        identityBus.unregisterService(QStringLiteral("ru.omp.am"));
        identityBus.unregisterObject(QStringLiteral("/ru/omp/am"));
        QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-destruction-identity"));
    }
    check(bus.unregisterService(QString::fromLatin1(service)), "release manager name");
    QDBusConnection::disconnectFromBus(QStringLiteral("microg-test-other"));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(service), QString::fromLatin1(path),
        QString::fromLatin1(interface), QStringLiteral("GetProvider"));
    QDBusMessage badResponse = call.createReply(QVariantList() << QStringLiteral("unexpected"));
    QDBusPendingReply<QString> badReply(QDBusPendingCall::fromCompletedCall(badResponse));
    check(!readSharedMicrogProvider(badReply, &provider, &error) && !error.isEmpty(),
          "reject malformed provider in reply");
    check(!QFile::exists(marker), "manager activation did not run");
    bus.interface()->startService(QString::fromLatin1(service));
    check(QFile::exists(marker), "activation fixture is discoverable by the bus");
    std::puts("shared microG IPC tests passed");
    return 0;
}
