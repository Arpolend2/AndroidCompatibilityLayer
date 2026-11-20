#include "../memory_log_bus.h"
#include "../manager_identity.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

static bool checkManagerExecutablePaths()
{
    const QString installed = QStringLiteral(
        "/opt/app/ru.androidcompatibilitylayer.appmanager/0.1.0-1/bin/ru.androidcompatibilitylayer.appmanager");
    const QStringList rejected = {
        QStringLiteral("/usr/bin/unrelated"),
        QStringLiteral("/opt/app/ru.example.other/0.1.0-1/bin/ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/opt/app/ru.androidcompatibilitylayer.appmanager/../bin/ru.androidcompatibilitylayer.appmanager"),
        QStringLiteral("/opt/app/ru.androidcompatibilitylayer.appmanager/0.1.0-1/extra/bin/ru.androidcompatibilitylayer.appmanager"),
        installed + QLatin1Char('\n')
    };
    if (!isTrustedAppManagerExecutable(installed) ||
        !isTrustedAppManagerExecutable(QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.appmanager"))) {
        std::fprintf(stderr, "Expected manager executable was rejected\n");
        return false;
    }
    for (const QString &path : rejected) {
        if (isTrustedAppManagerExecutable(path)) {
            std::fprintf(stderr, "Unexpected manager executable was accepted: %s\n", qPrintable(path));
            return false;
        }
    }
    return true;
}

class Identity : public QDBusVirtualObject
{
public:
    bool accept = true;
    bool acceptManager = true;
    bool acceptWellKnownManager = false;
    bool failManager = false;
    QString managerKey = QStringLiteral("test-key");
    bool failSender = false;
    QString senderPath = QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    QString senderKey = QStringLiteral("test-key");
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.member() != QLatin1String("IdentifyByBusName")) return false;
        const QDBusReply<QString> manager = connection.interface()->serviceOwner(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        const bool wellKnown = message.arguments().size() == 1 &&
            message.arguments().at(0).toString() == QLatin1String("ru.androidcompatibilitylayer.appmanager");
        const bool identifyingManager = manager.isValid() && message.arguments().size() == 1 &&
            (wellKnown || message.arguments().at(0).toString() == manager.value());
        if ((identifyingManager && failManager) || (!identifyingManager && failSender)) {
            connection.send(message.createErrorReply(QDBusError::AccessDenied,
                                                     QStringLiteral("PRIVATE-ERROR-CONTENT")));
            return true;
        }
        const QString path = identifyingManager && (acceptManager || (wellKnown && acceptWellKnownManager))
            ? QStringLiteral("/opt/app/ru.androidcompatibilitylayer.appmanager/0.1.0-1/bin/ru.androidcompatibilitylayer.appmanager") : accept
            ? senderPath
            : QStringLiteral("/usr/bin/unrelated");
        connection.send(message.createReply(QVariantList() << path <<
                                             (identifyingManager ? managerKey : senderKey)));
        return true;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2) {
        if (QByteArray(argv[1]) == "append-denied") {
            QDBusMessage request = QDBusMessage::createMethodCall(
                QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
                QStringLiteral("/ru/androidcompatibilitylayer/appmanager/log"),
                QStringLiteral("ru.androidcompatibilitylayer.appmanager.log"), QStringLiteral("Append"));
            request << QStringLiteral("test") << QStringLiteral("event") << false << QVariant::fromValue<qulonglong>(0);
            return QDBusConnection::sessionBus().call(request).errorName() ==
                   QLatin1String("org.freedesktop.DBus.Error.AccessDenied") ? 0 : 1;
        }
        if (QByteArray(argv[1]) == "invalid-diagnostic") {
            QDBusMessage request = QDBusMessage::createMethodCall(
                QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
                QStringLiteral("/ru/androidcompatibilitylayer/appmanager/log"),
                QStringLiteral("ru.androidcompatibilitylayer.appmanager.log"), QStringLiteral("Diagnostic"));
            request << QStringLiteral("PRIVATE-DIAGNOSTIC") << QStringLiteral("started");
            return QDBusConnection::sessionBus().call(request).type() == QDBusMessage::ErrorMessage ? 0 : 1;
        }
        if (QByteArray(argv[1]) == "flood") {
            QDBusConnection bus = QDBusConnection::sessionBus();
            int refused = 0;
            for (int i = 0; i < 140; ++i) {
                QDBusMessage request = QDBusMessage::createMethodCall(
                    QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
                    QStringLiteral("/ru/androidcompatibilitylayer/appmanager/log"),
                    QStringLiteral("ru.androidcompatibilitylayer.appmanager.log"), QStringLiteral("Append"));
                request << QStringLiteral("test") << QStringLiteral("event") << false << QVariant::fromValue<qulonglong>(0);
                if (bus.call(request).type() == QDBusMessage::ErrorMessage) ++refused;
            }
            if (!refused) return 1;
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
            return app.exec();
        }
        QString error;
        if (!MemoryLogBus::startClient(&error, QByteArray(argv[1]) != "no-append")) return 1;
        if (QByteArray(argv[1]) == "no-append") {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 5000 && !MemoryLogBus::clientDetailedEnabled()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            if (!MemoryLogBus::clientDetailedEnabled()) return 1;
            MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("shared snapshot event"),
                                 QString(), false);
            MemoryLogBus::enqueue(QStringLiteral("test"), QStringLiteral("queued event"), false);
            QTimer::singleShot(300, &app, &QCoreApplication::quit);
            return app.exec();
        }
        if (QByteArray(argv[1]) == "disabled") {
            QTimer::singleShot(1500, &app, [&app]() {
                MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("disabled private event"),
                                     QString(), false);
                QTimer::singleShot(300, &app, &QCoreApplication::quit);
            });
            return app.exec();
        }
        if (QByteArray(argv[1]) == "toggle-cycle") {
            auto waitForState = [](bool enabled, int timeoutMs) {
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < timeoutMs) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    if (MemoryLogBus::clientDetailedEnabled() == enabled) return true;
                    QThread::msleep(5);
                }
                return false;
            };
            const QStringList messages = {
                QStringLiteral("cycle initially enabled"),
                QStringLiteral("cycle disabled one"),
                QStringLiteral("cycle enabled one"),
                QStringLiteral("cycle disabled two"),
                QStringLiteral("cycle enabled two")
            };
            const bool states[] = {true, false, true, false, true};
            for (int i = 0; i < 5; ++i) {
                if (!waitForState(states[i], 4000)) {
                    std::fprintf(stderr, "toggle-cycle timed out at step %d; state=%d\n",
                                 i, MemoryLogBus::clientDetailedEnabled());
                    return 1;
                }
                MemoryLogBus::record(QStringLiteral("test"), messages.at(i), QString(), false);
            }
            QTimer::singleShot(750, &app, &QCoreApplication::quit);
            return app.exec();
        }
        if (QByteArray(argv[1]) == "owner-loss") {
            auto waitForState = [](bool enabled, int timeoutMs) {
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < timeoutMs) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    if (MemoryLogBus::clientDetailedEnabled() == enabled) return true;
                    QThread::msleep(5);
                }
                return false;
            };
            if (!waitForState(true, 5000) || !waitForState(false, 5000)) return 1;
            MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("owner loss event"), QString(), false);
            if (!waitForState(true, 8000)) return 1;
            MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("owner restored event"), QString(), false);
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
            return app.exec();
        }
        if (QByteArray(argv[1]) == "early") {
            if (MemoryLogBus::clientDetailedEnabled()) return 1;
            MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("PRIVATE-EARLY"),
                                 QString(), false);
            QTimer::singleShot(500, []() {
                if (!MemoryLogBus::clientDetailedEnabled()) std::exit(1);
                MemoryLogBus::record(QStringLiteral("test"), QStringLiteral("PRIVATE-AFTER"),
                                     QString(), false);
            });
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
            return app.exec();
        }
        QElapsedTimer stateTimer;
        stateTimer.start();
        while (stateTimer.elapsed() < 5000 && !MemoryLogBus::clientDetailedEnabled()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        if (!MemoryLogBus::clientDetailedEnabled()) return 1;
        for (int i = 0; i < 1000; ++i)
            MemoryLogBus::enqueue(QStringLiteral("test"), QString(1000, QLatin1Char('x')), false);
        QTimer::singleShot(2000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (!checkManagerExecutablePaths()) return 1;
    QDBusConnection bus = QDBusConnection::sessionBus();
    Identity identity;
    if (!bus.isConnected() || !bus.registerService(QStringLiteral("ru.omp.am")) ||
        !bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) ||
        !bus.registerService(QStringLiteral("ru.androidcompatibilitylayer.appmanager"))) return 1;
    quint64 dropped = 0;
    int received = 0;
    QStringList messages;
    bool extended = true;
    QString error;
    if (!MemoryLogBus::startServer([&](const QString &, const QString &, const QString &message, bool, quint64 lost) {
            ++received;
            dropped += lost;
            messages << message;
        }, [&]() { return extended; }, &error)) return 1;
    QByteArray childErrors;
    auto runChild = [&](const QString &mode = QStringLiteral("child"), const QString &address = QString()) {
        QProcess child;
        if (!address.isEmpty()) {
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), address);
            child.setProcessEnvironment(environment);
        }
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&child, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                         &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        child.start(app.applicationFilePath(), QStringList() << mode);
        timeout.start(mode == QLatin1String("toggle-cycle") || mode == QLatin1String("owner-loss")
                      ? 15000 : 10000);
        loop.exec();
        childErrors = child.readAllStandardError();
        const bool succeeded = child.state() == QProcess::NotRunning && child.exitCode() == 0;
        if (!succeeded)
            std::fprintf(stderr, "child mode=%s state=%d exit=%d status=%d stderr=%s\n",
                          qPrintable(mode), child.state(), child.exitCode(), child.exitStatus(),
                          childErrors.constData());
        return succeeded;
    };
    if (!runChild(QStringLiteral("early")) ||
        !messages.contains(QStringLiteral("Log transport: 1 early events unavailable while detailed state was unknown")) ||
        messages.contains(QStringLiteral("PRIVATE-EARLY")) || !messages.contains(QStringLiteral("PRIVATE-AFTER"))) {
        std::fprintf(stderr, "early detailed events were not accounted for correctly\n");
        return 1;
    }
    const int beforeSnapshotOnly = received;
    if (!runChild(QStringLiteral("no-append")) || received != beforeSnapshotOnly) {
        std::fprintf(stderr, "snapshot-only client sent log content over D-Bus Append\n");
        return 1;
    }
    QTimer::singleShot(1000, [&]() { extended = false; });
    QTimer::singleShot(2500, [&]() { extended = true; });
    QTimer::singleShot(4000, [&]() { extended = false; });
    QTimer::singleShot(5500, [&]() { extended = true; });
    if (!runChild(QStringLiteral("toggle-cycle")) ||
        !messages.contains(QStringLiteral("cycle initially enabled")) ||
        messages.contains(QStringLiteral("cycle disabled one")) ||
        !messages.contains(QStringLiteral("cycle enabled one")) ||
        messages.contains(QStringLiteral("cycle disabled two")) ||
        !messages.contains(QStringLiteral("cycle enabled two"))) {
        std::fprintf(stderr, "detailed state did not follow repeated off/on changes; received=%d child=%s\n",
                    received, childErrors.constData());
        for (const QString &message : messages)
           std::fprintf(stderr, "message=%s\n", qPrintable(message));
        return 1;
    }
    bool ownerReleased = false;
    bool ownerRestored = false;
    QTimer::singleShot(1000, [&]() {
        ownerReleased = bus.unregisterService(QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        extended = false;
    });
    QTimer::singleShot(3500, [&]() {
        ownerRestored = bus.registerService(QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        extended = true;
    });
    if (!runChild(QStringLiteral("owner-loss")) || !ownerReleased || !ownerRestored ||
        messages.contains(QStringLiteral("owner loss event")) ||
        !messages.contains(QStringLiteral("owner restored event"))) {
        std::fprintf(stderr, "manager owner loss did not stop logging and reconnect safely\n");
        return 1;
    }
    const QString legacySender = identity.senderPath;
    const QString managerSender = QStringLiteral(
        "/opt/app/ru.androidcompatibilitylayer.appmanager/0.1.0-1/bin/ru.androidcompatibilitylayer.appmanager");
    const QString standaloneSender = QStringLiteral(
        "/opt/app/ru.androidcompatibilitylayer.Android0123456789abcdef01234567"
        "/0.1.0-1/bin/ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    identity.senderKey = QString(256, QLatin1Char('x'));
    for (const QString &path : {managerSender, standaloneSender}) {
        identity.senderPath = path;
        const int before = messages.count(QStringLiteral("PRIVATE-AFTER"));
        if (!runChild(QStringLiteral("early")) || messages.count(QStringLiteral("PRIVATE-AFTER")) != before + 1) {
            std::fprintf(stderr, "versioned sender did not deliver the detailed event\n");
            return 1;
        }
    }
    identity.senderPath = standaloneSender.left(standaloneSender.size() - 1) + QLatin1Char('8');
    if (!runChild(QStringLiteral("append-denied"))) {
        std::fprintf(stderr, "mismatched standalone package and executable were accepted\n");
        return 1;
    }
    identity.senderPath = standaloneSender;
    for (const QString &key : {QString(), QString(257, QLatin1Char('x'))}) {
        identity.senderKey = key;
        if (!runChild(QStringLiteral("append-denied"))) {
            std::fprintf(stderr, "Versioned sender with invalid key length %d was not denied\n", key.size());
            return 1;
        }
    }
    identity.senderPath = legacySender;
    identity.senderKey = QStringLiteral("test-key");
    if (qEnvironmentVariableIsSet("ACL_TEST_DBUS_PROXY")) {
        QTemporaryDir temporary;
        if (!temporary.isValid()) return 1;
        const QString socket = temporary.path() + QStringLiteral("/bus");
        QProcess proxy;
        proxy.start(QStringLiteral("xdg-dbus-proxy"),
                    QStringList() << QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")) << socket
                                  << QStringLiteral("--filter") << QStringLiteral("--log")
                                  << QStringLiteral("--talk=ru.androidcompatibilitylayer.appmanager")
                                  << QStringLiteral("--talk=ru.omp.am"));
        if (!proxy.waitForStarted(5000)) return 1;
        for (int i = 0; i < 100 && !QFileInfo::exists(socket); ++i) QThread::msleep(20);
        const int beforeProxy = received;
        const bool ok = runChild(QStringLiteral("early"), QStringLiteral("unix:path=") + socket);
        proxy.terminate();
        proxy.waitForFinished(5000);
        if (!ok || received <= beforeProxy) {
            std::fprintf(stderr, "proxy test: %s\n%s\n", childErrors.constData(),
                         proxy.readAllStandardOutput().constData());
            return 1;
        }
    }
    if (!runChild() || !received || !dropped) {
        std::fprintf(stderr, "accepted: received=%d dropped=%llu\n", received,
                     static_cast<unsigned long long>(dropped));
        return 1;
    }
    const quint64 beforeFlood = dropped;
    if (!runChild(QStringLiteral("flood")) || dropped <= beforeFlood) {
        std::fprintf(stderr, "rate-limited events were not reported\n");
        return 1;
    }
    identity.accept = false;
    int before = received;
    if (!runChild() || received != before ||
        MemoryLogBus::statistics().contains(QStringLiteral("identity rejected=0"))) {
        std::fprintf(stderr, "rejected: before=%d received=%d\n", before, received);
        return 1;
    }
    identity.accept = true;
    identity.failSender = true;
    if (!runChild() || received != before ||
        MemoryLogBus::statistics().contains(QStringLiteral("identification errors=0")) ||
        !childErrors.contains("Log IPC event delivery: access denied") ||
        childErrors.contains("PRIVATE-ERROR-CONTENT")) {
        std::fprintf(stderr, "sender identity errors were not safely reported\n");
        return 1;
    }
    identity.failSender = false;
    identity.acceptManager = false;
    identity.acceptWellKnownManager = true;
    const bool identityChildResult = runChild();
    const QString expectedIdentity = QStringLiteral(
        "\"executable\":\"/usr/bin/ru.androidcompatibilitylayer.Android0123456789abcdef01234567\"");
    QElapsedTimer identityDetailTimer;
    identityDetailTimer.start();
    bool identityPayload = false;
    while (identityDetailTimer.elapsed() < 1000 && !identityPayload) {
        for (const QString &message : messages)
           if (message.contains(expectedIdentity)) identityPayload = true;
        if (!identityPayload) {
           QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
           QThread::msleep(5);
        }
    }
    const bool identityChecks = received > before &&
        childErrors.contains("Log IPC manager identification: identity rejected: executable mismatch, key present") &&
        childErrors.contains("Log IPC manager well-known comparison: expected=1; executable_equal=0; key_equal=1") &&
        MemoryLogBus::statistics().contains(QStringLiteral("manager identification / identity rejected: executable mismatch, key present = ")) &&
        !childErrors.contains("test-key") && !childErrors.contains("/usr/bin/") &&
        identityPayload;
    if (!identityChildResult || !identityChecks) {
        std::fprintf(stderr, "trusted well-known manager identity was not safely confirmed; before=%d received=%d errors=%s stats=%s last=%s\n",
                     before, received, childErrors.constData(),
                     qPrintable(MemoryLogBus::statistics()),
                     qPrintable(messages.isEmpty() ? QString() : messages.last().left(160)));
        std::fprintf(stderr, "child=%d checks=%d diag=%d comparison=%d statistic=%d redacted=%d payload=%d\n",
                     identityChildResult, identityChecks,
                     childErrors.contains("Log IPC manager identification: identity rejected: executable mismatch, key present"),
                     childErrors.contains("Log IPC manager well-known comparison: expected=1; executable_equal=0; key_equal=1"),
                     MemoryLogBus::statistics().contains(QStringLiteral("manager identification / identity rejected: executable mismatch, key present = ")),
                     !childErrors.contains("test-key") && !childErrors.contains("/usr/bin/"),
                     identityPayload);
        return 1;
    }
    extended = false;
    identity.accept = false;
    const int beforeIdentityDetails = received;
    if (!runChild(QStringLiteral("disabled")) || received != beforeIdentityDetails ||
        messages.join(QLatin1Char('\n')).contains(QStringLiteral("/usr/bin/unrelated"))) {
        std::fprintf(stderr, "identity details escaped while extended logging was disabled\n");
        return 1;
    }
    extended = true;
    if (!runChild() || received <= beforeIdentityDetails ||
        !messages.join(QLatin1Char('\n')).contains(QStringLiteral("\"executable\":\"/usr/bin/unrelated\"")) ||
        !messages.join(QLatin1Char('\n')).contains(QStringLiteral("\"keyid\":\"test-key\""))) {
        std::fprintf(stderr, "extended logging did not retain the complete manager identity response\n");
        return 1;
    }
    before = received;
    identity.accept = true;
    identity.acceptManager = true;
    identity.managerKey.clear();
    if (!runChild(QStringLiteral("disabled")) || received != before + 1 ||
        !childErrors.contains("identity rejected: key empty") ||
        !MemoryLogBus::statistics().contains(QStringLiteral("identity rejected: key empty = "))) {
        std::fprintf(stderr, "empty manager key was not reported\n");
        return 1;
    }
    before = received;
    identity.acceptManager = false;
    if (!runChild(QStringLiteral("disabled")) || received != before + 1 ||
        !childErrors.contains("identity rejected: executable mismatch, key empty") ||
        !MemoryLogBus::statistics().contains(QStringLiteral("identity rejected: executable mismatch, key empty = "))) {
        std::fprintf(stderr, "empty key with executable mismatch was not reported\n");
        return 1;
    }
    before = received;
    identity.acceptManager = true;
    identity.managerKey = QString(257, QLatin1Char('x'));
    if (!runChild(QStringLiteral("disabled")) || received != before ||
        !childErrors.contains("identity rejected: key oversized") ||
        childErrors.contains(identity.managerKey.toUtf8())) {
        std::fprintf(stderr, "oversized manager key was not safely reported\n");
        return 1;
    }
    identity.acceptManager = false;
    if (!runChild(QStringLiteral("disabled")) || received != before ||
        !childErrors.contains("identity rejected: executable mismatch, key oversized")) {
        std::fprintf(stderr, "oversized key with executable mismatch was not reported\n");
        return 1;
    }
    identity.acceptManager = true;
    identity.managerKey = QStringLiteral("test-key");
    identity.failManager = true;
    if (!runChild(QStringLiteral("disabled")) || received != before ||
        !MemoryLogBus::statistics().contains(QStringLiteral("manager identification / access denied = ")) ||
        MemoryLogBus::statistics().contains(QStringLiteral("PRIVATE-ERROR-CONTENT"))) {
        std::fprintf(stderr, "manager identification failure was not safely reported\n");
        return 1;
    }
    if (!runChild(QStringLiteral("invalid-diagnostic")) ||
        MemoryLogBus::statistics().contains(QStringLiteral("PRIVATE-DIAGNOSTIC"))) {
        std::fprintf(stderr, "invalid diagnostic was accepted or exposed\n");
        return 1;
    }
    std::puts("Memory log bus OK");
    return 0;
}
