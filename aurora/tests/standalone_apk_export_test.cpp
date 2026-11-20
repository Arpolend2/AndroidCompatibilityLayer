#include "../standalone_apk_export.h"
#include "../external_apk_install.h"
#include "../library_app_index.h"
#include "../standalone_app_mapping.h"
#include "../../core/src/apk/mg_apk.h"

#include <QDesktopServices>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <functional>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <utime.h>

static QStringList identityWarnings;
static QStringList launchWarnings;
static std::atomic<bool> nativeTimerWarning(false);
static QtMessageHandler previousMessageHandler = nullptr;

static void captureIdentityWarning(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.contains(QLatin1String("Manager identity could not be verified")))
        identityWarnings.append(message);
    if (type == QtWarningMsg && message.contains(QLatin1String("App launch failed:")))
        launchWarnings.append(message);
    if (type == QtWarningMsg && message.contains(QLatin1String("Timers can only be used with threads started with QThread")))
        nativeTimerWarning.store(true);
    if (previousMessageHandler) previousMessageHandler(type, context, message);
}

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != "test APK") return false;
    std::memset(info, 0, sizeof(*info));
    std::strcpy(info->package, "org.example.app");
    std::strcpy(info->label, "Example");
    info->version_code = 42;
    info->runnable = true;
    return true;
}

class Handler : public QObject
{
    Q_OBJECT
public:
    QString path;
public slots:
    void open(const QUrl &url) { path = url.toLocalFile(); emit opened(path); }
signals:
    void opened(const QString &path);
};

class FakeManager : public QDBusVirtualObject
{
public:
    explicit FakeManager(const QDBusConnection &connection) : bus(connection) {}
    QDBusConnection bus;
    QString result;
    QString id;
    QString apps = QStringLiteral("MEGLER_INSTALL_STATUS_V1\norg.example.library\torg.example.library.Main\tLibrary\t1\t42\n");
    bool trusted = true;
    QString key = QStringLiteral("test-key");
    QString launched;
    QString launchResult = QStringLiteral("ok");
    bool holdLaunchReply = false;
    bool hasDelayedLaunch = false;
    int identityChecks = 0;
    QDBusMessage delayedLaunch;
    void replyToLaunch(const QString &result)
    {
        if (!hasDelayedLaunch) return;
        hasDelayedLaunch = false;
        bus.send(delayedLaunch.createReply(QVariantList() << result));
    }
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() == QLatin1String("ru.omp.am.Identify") &&
            message.member() == QLatin1String("IdentifyByBusName")) {
            ++identityChecks;
            connection.send(message.createReply(QVariantList()
                << (trusted ? QStringLiteral("/opt/app/ru.androidcompatibilitylayer.appmanager/0.1.0-1/bin/ru.androidcompatibilitylayer.appmanager")
                            : QStringLiteral("/usr/bin/untrusted"))
                << key));
            return true;
        }
        if (message.interface() == QLatin1String("ru.androidcompatibilitylayer.appmanager") &&
            message.member() == QLatin1String("storeResult")) {
            connection.send(message.createReply(QVariantList()
                << (message.arguments().value(0).toString() == id ? result : QString())));
            return true;
        }
        if (message.interface() == QLatin1String("ru.androidcompatibilitylayer.appmanager") &&
            message.member() == QLatin1String("libraryApps")) {
            connection.send(message.createReply(QVariantList() << apps));
            return true;
        }
        if (message.interface() == QLatin1String("ru.androidcompatibilitylayer.appmanager") &&
            message.member() == QLatin1String("launchApp")) {
            launched = message.arguments().value(0).toString();
            if (holdLaunchReply) {
                message.setDelayedReply(true);
                delayedLaunch = message;
                hasDelayedLaunch = true;
            }
            else connection.send(message.createReply(QVariantList() << launchResult));
            return true;
        }
        return false;
    }
};

struct NativeLaunch {
    megler_system_host host;
    const char *package;
    bool result = false;
    std::atomic<bool> done{false};
};

static void *runNativeLaunch(void *data)
{
    auto *launch = static_cast<NativeLaunch *>(data);
    launch->result = launch->host.launch_app(launch->host.user, launch->package, nullptr);
    launch->done.store(true);
    return nullptr;
}

static bool waitUntil(const std::function<bool()> &condition, int timeout = 3000)
{
    if (condition()) return true;
    QEventLoop loop;
    QTimer poll;
    bool satisfied = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (condition()) {
            satisfied = true;
            loop.quit();
        }
    });
    poll.start(10);
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    loop.exec();
    return satisfied;
}

class LaunchThread : public QThread
{
public:
    megler_system_host host;
    bool result = false;
    void run() override
    {
        result = host.launch_app(host.user, "org.example.library", nullptr);
    }
};

static bool importHandoff(const QString &path, const QString &store, bool accept)
{
    ExternalApkInstall installer(store);
    QEventLoop loop;
    bool prompted = false;
    bool completed = false;
    bool installed = false;
    QObject::connect(&installer, &ExternalApkInstall::confirmationRequested, &loop,
                     [&](const QString &, const QString &, quint32) {
        prompted = true;
        if (accept) {
            QFile shared(path);
            if (!shared.open(QIODevice::WriteOnly | QIODevice::Truncate) || shared.write("modified") != 8) {
                loop.quit();
                return;
            }
        }
        installer.decision(accept);
    });
    QObject::connect(&installer, &ExternalApkInstall::finished, &loop,
                     [&](bool ok, const QString &message) {
        completed = true;
        installed = ok;
        if (ok != accept) std::fprintf(stderr, "Import failed: %s\n", qPrintable(message));
        loop.quit();
    });
    QString error;
    if (!installer.start(path, &error)) {
        std::fprintf(stderr, "Import could not start: %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(10000, &loop, SLOT(quit()));
    loop.exec();
    if (!prompted || !completed || installed != accept) return false;
    QFile imported(QDir(store).filePath(QStringLiteral("org.example.app/base.apk")));
    if (!accept) return !imported.exists();
    return imported.open(QIODevice::ReadOnly) && imported.readAll() == "test APK";
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    qputenv("HOME", QFile::encodeName(root.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root.path()));
    QGuiApplication app(argc, argv);
    const QString indexedRoot = QDir(root.path()).filePath(QStringLiteral("indexed-library"));
    LibraryAppIndex emptyIndex(indexedRoot);
    emptyIndex.start();
    if (!emptyIndex.wait(5000) || !emptyIndex.error.isEmpty() ||
        emptyIndex.contents != QLatin1String("MEGLER_INSTALL_STATUS_V1\n")) return 1;
    const QString indexedApp = QDir(indexedRoot).filePath(QStringLiteral("org.example.app"));
    if (!QDir().mkpath(indexedApp)) return 1;
    const QString indexedApk = QDir(indexedApp).filePath(QStringLiteral("base.apk"));
    QFile indexedFile(indexedApk);
    if (!indexedFile.open(QIODevice::WriteOnly) || indexedFile.write("test APK") != 8) return 1;
    indexedFile.close();
    LibraryAppIndex addedIndex(indexedRoot);
    addedIndex.start();
    if (!addedIndex.wait(5000) || !addedIndex.error.isEmpty() ||
        !addedIndex.contents.contains(QLatin1String("org.example.app\t\tExample\t\t42\n"))) return 1;
    if (!indexedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) || indexedFile.write("bad") != 3) return 1;
    indexedFile.close();
    LibraryAppIndex brokenIndex(indexedRoot);
    brokenIndex.start();
    if (!brokenIndex.wait(5000) || brokenIndex.error.isEmpty() || !brokenIndex.contents.isEmpty()) return 1;
    if (!indexedFile.remove() || !QDir().rmdir(indexedApp)) return 1;
    LibraryAppIndex removedIndex(indexedRoot);
    removedIndex.start();
    if (!removedIndex.wait(5000) || !removedIndex.error.isEmpty() ||
        removedIndex.contents != emptyIndex.contents) return 1;
    const QString mappingDir = QDir(root.path()).filePath(QStringLiteral("mapping"));
    if (!QDir().mkpath(mappingDir) || !StandaloneAppMapping::read(mappingDir).isEmpty()) return 1;
    const QString firstId = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    const QString secondId = QStringLiteral("ru.androidcompatibilitylayer.Android1123456789abcdef01234567");
    const QString firstRpm = QDir(mappingDir).filePath(firstId + QStringLiteral("-1.0.0-1.x86_64.rpm"));
    const QString secondRpm = QDir(mappingDir).filePath(secondId + QStringLiteral("-1.0.0-1.x86_64.rpm"));
    QFile firstFile(firstRpm), secondFile(secondRpm);
    if (!firstFile.open(QIODevice::WriteOnly) || !secondFile.open(QIODevice::WriteOnly)) return 1;
    firstFile.close();
    secondFile.close();
    if (!StandaloneAppMapping::save(mappingDir, firstRpm) ||
        StandaloneAppMapping::read(mappingDir) != firstId ||
        !StandaloneAppMapping::save(mappingDir, secondRpm) ||
        StandaloneAppMapping::read(mappingDir) != secondId ||
        StandaloneAppMapping::save(mappingDir, QDir(root.path()).filePath(QFileInfo(firstRpm).fileName())) ||
        StandaloneAppMapping::read(mappingDir) != secondId) return 1;
    QFile badMapping(QDir(mappingDir).filePath(QStringLiteral(".last-created-app-id")));
    if (!badMapping.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        badMapping.write("bad") != 3) return 1;
    badMapping.close();
    if (!StandaloneAppMapping::read(mappingDir).isEmpty()) return 1;
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                        QStringLiteral("fake-manager"));
    FakeManager manager(bus);
    if (!bus.registerVirtualObject(QStringLiteral("/ru/androidcompatibilitylayer/appmanager"), &manager) ||
        !bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &manager) ||
        !bus.registerService(QStringLiteral("ru.androidcompatibilitylayer.appmanager")) ||
        !bus.registerService(QStringLiteral("ru.omp.am"))) return 1;
    QString initialError;
    const QString emptySharedRoot = QDir(root.path()).filePath(QStringLiteral("new-organization/common"));
    if (!StandaloneApkExport::takeNext(emptySharedRoot, &initialError).isEmpty() ||
        !initialError.isEmpty() ||
        !QDir(QDir(emptySharedRoot).filePath(QStringLiteral("store-install-handoffs"))).exists())
        return 1;
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QStringList before = QDir(downloads).entryList(QDir::Files);
    Handler handler;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &handler, "open");
    megler_system_host other = {};
    {
        previousMessageHandler = qInstallMessageHandler(captureIdentityWarning);
        StandaloneApkExport storeExporter(root.path(), root.path(), other, nullptr, QString(),
                                         StandaloneApkExport::QueueLaunch);
        const auto storeCallbacks = storeExporter.host();
        manager.holdLaunchReply = true;
        NativeLaunch launch;
        launch.host = storeCallbacks;
        launch.package = "org.example.storelaunch";
        pthread_t thread;
        if (pthread_create(&thread, nullptr, runNativeLaunch, &launch) != 0 ||
            pthread_join(thread, nullptr) != 0 || !storeExporter.error().isEmpty() || !launch.result ||
            storeCallbacks.launch_app(storeCallbacks.user, "org.example.duplicate", nullptr)) {
            std::fprintf(stderr, "Native Store launch did not queue once without waiting for a reply\n");
            return 1;
        }
        if (!waitUntil([&]() { return manager.hasDelayedLaunch; }) ||
            manager.launched != QLatin1String("org.example.storelaunch") || nativeTimerWarning.load())
            return 1;
        const int beforeIdentity = manager.identityChecks;
        manager.replyToLaunch(QStringLiteral("error:runtime unavailable"));
        if (!waitUntil([&]() { return !launchWarnings.isEmpty(); }) ||
            manager.identityChecks <= beforeIdentity ||
            !launchWarnings.last().contains(QLatin1String("error:runtime unavailable"))) {
            std::fprintf(stderr, "Delayed launch failure was not verified and reported: identity=%d before=%d warning=%s\n",
                         manager.identityChecks, beforeIdentity,
                         launchWarnings.isEmpty() ? "(none)" : qPrintable(launchWarnings.last()));
            return 1;
        }
        manager.trusted = false;
        manager.holdLaunchReply = false;
        manager.launched.clear();
        launchWarnings.clear();
        if (!storeCallbacks.launch_app(storeCallbacks.user, "org.example.untrusted", nullptr) ||
            !waitUntil([&]() { return !launchWarnings.isEmpty(); }) ||
            !launchWarnings.last().contains(QLatin1String("identity could not be verified"))) {
            std::fprintf(stderr, "Untrusted queued launch result was accepted\n");
            return 1;
        }
        manager.trusted = true;
        manager.launched.clear();
        launchWarnings.clear();
        manager.holdLaunchReply = true;
        if (!storeCallbacks.launch_app(storeCallbacks.user, "org.example.success", nullptr) ||
            !waitUntil([&]() { return manager.hasDelayedLaunch; })) return 1;
        manager.replyToLaunch(QStringLiteral("ok"));
        if (!waitUntil([&]() {
                return storeCallbacks.launch_app(storeCallbacks.user, "org.example.next", nullptr);
            }) || !waitUntil([&]() { return manager.hasDelayedLaunch; })) {
            std::fprintf(stderr, "Verified successful launch did not release the queue\n");
            return 1;
        }
        manager.replyToLaunch(QStringLiteral("ok"));
        manager.launched.clear();
        auto *doomed = new StandaloneApkExport(root.path(), root.path(), other, nullptr, QString(),
                                               StandaloneApkExport::QueueLaunch);
        const auto doomedCallbacks = doomed->host();
        if (!doomedCallbacks.launch_app(doomedCallbacks.user, "org.example.destroyed", nullptr) ||
            !waitUntil([&]() { return manager.hasDelayedLaunch; })) return 1;
        delete doomed;
        manager.replyToLaunch(QStringLiteral("ok"));
        QEventLoop afterDestroy;
        QTimer::singleShot(200, &afterDestroy, &QEventLoop::quit);
        afterDestroy.exec();
        if (!launchWarnings.isEmpty()) return 1;
        manager.holdLaunchReply = false;
        manager.launched.clear();
        qInstallMessageHandler(previousMessageHandler);
        previousMessageHandler = nullptr;
        identityWarnings.clear();
    }
    {
        manager.holdLaunchReply = true;
        auto *waitingExporter = new StandaloneApkExport(root.path(), root.path(), other);
        const auto waitingHost = waitingExporter->host();
        if (waitingHost.launch_app(waitingHost.user, "org.example.ui", nullptr)) return 1;
        QEventLoop checkNoLaunch;
        QTimer::singleShot(100, &checkNoLaunch, &QEventLoop::quit);
        checkNoLaunch.exec();
        if (!manager.launched.isEmpty()) {
            std::fprintf(stderr, "Rejected UI-thread launch was still sent\n");
            return 1;
        }
        NativeLaunch launch;
        launch.host = waitingHost;
        launch.package = "org.example.waiting";
        pthread_t thread;
        if (pthread_create(&thread, nullptr, runNativeLaunch, &launch) != 0 ||
            !waitUntil([&]() { return manager.hasDelayedLaunch; }) || launch.done.load()) return 1;
        manager.replyToLaunch(QStringLiteral("error:runtime unavailable"));
        if (!waitUntil([&]() { return launch.done.load(); }) || pthread_join(thread, nullptr) != 0 ||
            launch.result) {
            std::fprintf(stderr, "Native waiting launch did not return the manager failure\n");
            return 1;
        }
        delete waitingExporter;
        manager.launched.clear();

        auto *doomed = new StandaloneApkExport(root.path(), root.path(), other);
        NativeLaunch pending;
        pending.host = doomed->host();
        pending.package = "org.example.cancelled";
        if (pthread_create(&thread, nullptr, runNativeLaunch, &pending) != 0 ||
            !waitUntil([&]() { return manager.hasDelayedLaunch; }) || pending.done.load()) return 1;
        delete doomed;
        if (!waitUntil([&]() { return pending.done.load(); }) || pthread_join(thread, nullptr) != 0 ||
            pending.result) {
            std::fprintf(stderr, "Destroying exporter did not release a waiting launch\n");
            return 1;
        }
        manager.replyToLaunch(QStringLiteral("ok"));
        QEventLoop afterDestroy;
        QTimer::singleShot(200, &afterDestroy, &QEventLoop::quit);
        afterDestroy.exec();
        manager.holdLaunchReply = false;
        manager.launched.clear();
    }
    const QString cachePath = QDir(root.path()).filePath(QStringLiteral("library-cache"));
    StandaloneApkExport exporter(root.path(), root.path(), other, nullptr, cachePath);
    if (!exporter.error().isEmpty()) return 1;
    const megler_system_host callbacks = exporter.host();
    const QByteArray name("request_0123456789abcdef0123456789abcdef.apk");
    const QByteArray mismatch("request_0123456789abcdef0123456789abcdef_43_org.example.app.apk");
    const QByteArray bundleName("request_0123456789abcdef0123456789abcdef_42_org.example.app.apks");
    const QByteArray xapkName("request_3123456789abcdef0123456789abcdef.xapk");
    const QByteArray tamperedName("request_1123456789abcdef0123456789abcdef.apk");
    const QByteArray declinedName("request_2123456789abcdef0123456789abcdef.apk");
    if (!callbacks.install_apk || callbacks.install_apk(callbacks.user, "/outside.apk", "../bad.apk"))
        return 1;
    if (!callbacks.install_apk(callbacks.user, "/outside.apk", name.constData()))
        return 1;
    const QString result = QDir(exporter.appsDir()).filePath(QStringLiteral("installs/") +
                                                            QString::fromLatin1(name) + QStringLiteral(".txt"));
    const QString source = QDir(root.path()).filePath(QStringLiteral("source.apk"));
    QFile apk(source);
    if (!apk.open(QIODevice::WriteOnly) || apk.write("test APK") != 8) return 1;
    apk.close();
    int stage = 0;
    bool submitted = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        const QString resultPath = stage == 1 || stage == 4 || stage == 5 || stage == 6 || stage == 7
            ? QDir(exporter.appsDir()).filePath(QStringLiteral("installs/") +
                QString::fromLatin1(stage == 1 ? mismatch : stage == 4 ? bundleName :
                                    stage == 5 ? tamperedName : stage == 6 ? declinedName : xapkName) + QStringLiteral(".txt"))
            : result;
        if ((stage == 2 || stage >= 4) && !submitted) {
            const QDir handoffs(QDir(root.path()).filePath(QStringLiteral("store-install-handoffs")));
            const QStringList names = handoffs.entryList(QStringList() <<
                (stage == 4 ? QStringLiteral("request_*.apks") :
                 stage == 7 ? QStringLiteral("request_*.xapk") : QStringLiteral("request_*.apk")), QDir::Files);
            if (names.isEmpty()) return;
            if (QFile::exists(resultPath) || names.size() != 1) {
                std::fprintf(stderr, "Handoff reported before manager import\n");
                app.exit(1);
                return;
            }
            QString error;
            const QString claimed = StandaloneApkExport::takeNext(root.path(), &error);
            QFile shared(claimed);
            const QByteArray expected = stage == 4 ? QByteArray("test bundle") :
                stage == 7 ? QByteArray("test XAPK") : QByteArray("test APK");
            if (claimed.isEmpty() || !error.isEmpty() ||
                !shared.open(QIODevice::ReadOnly) || shared.readAll() != expected) {
                app.exit(1);
                return;
            }
            shared.close();
            if (stage == 2 &&
                (!importHandoff(claimed, QDir(root.path()).filePath(QStringLiteral("library")), false) ||
                 !importHandoff(claimed, QDir(root.path()).filePath(QStringLiteral("library")), true))) {
                app.exit(1);
                return;
            }
            if (!StandaloneApkExport::removeClaimed(root.path(), claimed, &error) || !error.isEmpty()) {
                app.exit(1);
                return;
            }
            manager.id = QFileInfo(claimed).fileName().mid(8, 32);
            manager.result = QStringLiteral("installed:") + QString::fromLatin1(
                QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex());
            if (stage == 5) manager.result = QStringLiteral("installed:") + QString(64, QLatin1Char('0'));
            if (stage == 6) manager.result = QStringLiteral("failed");
            if (stage == 2) {
                manager.trusted = false;
                previousMessageHandler = qInstallMessageHandler(captureIdentityWarning);
                QTimer::singleShot(2500, &app, [&]() {
                    if (QFile::exists(result) || identityWarnings.isEmpty() ||
                        !identityWarnings.join(QLatin1Char('\n')).contains(
                            QLatin1String("executable=other, key=present")) ||
                        identityWarnings.join(QLatin1Char('\n')).contains(QLatin1String("/usr/bin/untrusted"))) {
                        std::fprintf(stderr, "Untrusted manager was accepted or not diagnosed safely\n");
                        app.exit(1);
                    }
                    manager.trusted = true;
                });
            }
            submitted = true;
            return;
        }
        QFile file(resultPath);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray status = file.readAll();
        file.close();
        if (!status.contains(stage == 2 || stage == 4 || stage == 7 ? "\tinstalled\t" : "\tfailed\t")) {
            std::fprintf(stderr, "Unexpected status: %s\n", status.constData());
            app.exit(1);
            return;
        }
        if (stage == 0) {
            if (!QFile::remove(result) ||
                !callbacks.install_apk(callbacks.user, QFile::encodeName(source).constData(), mismatch.constData())) {
                std::fprintf(stderr, "Could not submit second request\n");
                app.exit(1);
                return;
            }
            stage = 1;
        } else if (stage == 1) {
            if (!status.contains("does not match")) {
                std::fprintf(stderr, "Wrong session version was not rejected\n");
                app.exit(1);
                return;
            }
            if (!callbacks.install_apk(callbacks.user, QFile::encodeName(source).constData(), name.constData())) {
                app.exit(1);
                return;
            }
            stage = 2;
        } else if (stage == 2) {
            const QDir handoffs(QDir(root.path()).filePath(QStringLiteral("store-install-handoffs")));
            QFile apps(QDir(exporter.appsDir()).filePath(QStringLiteral("apps.txt")));
            if (!handler.path.isEmpty() || QDir(downloads).entryList(QDir::Files) != before ||
                !apps.open(QIODevice::ReadOnly) || !apps.readAll().contains("org.example.library\t")) {
                std::fprintf(stderr, "Unexpected file handoff\n");
                app.exit(1);
                return;
            }
            QString error;
            if (!StandaloneApkExport::takeNext(root.path(), &error).isEmpty() ||
                !error.isEmpty()) {
                std::fprintf(stderr, "Could not safely claim or remove handoff: %s\n", qPrintable(error));
                app.exit(1);
                return;
            }
            const QString stale = handoffs.filePath(QStringLiteral("request_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.apk"));
            QFile expired(stale);
            if (!expired.open(QIODevice::WriteOnly) || expired.write("old") != 3) {
                app.exit(1);
                return;
            }
            expired.close();
            const struct utimbuf oldTime = {1, 1};
            if (utime(QFile::encodeName(stale).constData(), &oldTime) != 0 ||
                !StandaloneApkExport::takeNext(root.path(), &error).isEmpty() ||
                !error.isEmpty() || QFile::exists(stale)) {
                std::fprintf(stderr, "Expired handoff was not removed: %s\n", qPrintable(error));
                app.exit(1);
                return;
            }
            const QString link = handoffs.filePath(QStringLiteral("request_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.apk"));
            if (symlink(QFile::encodeName(source).constData(), QFile::encodeName(link).constData()) != 0 ||
                !StandaloneApkExport::takeNext(root.path(), &error).isEmpty() ||
                error.isEmpty() || !QFile::exists(source) || !QFile::remove(link)) {
                std::fprintf(stderr, "Symlink was not rejected\n");
                app.exit(1);
                return;
            }
            if (linkat(AT_FDCWD, QFile::encodeName(source).constData(), AT_FDCWD,
                       QFile::encodeName(link).constData(), 0) != 0 ||
                !StandaloneApkExport::takeNext(root.path(), &error).isEmpty() ||
                error.isEmpty() || !QFile::exists(source) || !QFile::remove(link)) {
                std::fprintf(stderr, "Hard link was not rejected\n");
                app.exit(1);
                return;
            }
            for (int i = 0; i < 8; ++i) {
                QFile queued(handoffs.filePath(QStringLiteral("request_%1.apk").arg(i, 32, 16, QLatin1Char('0'))));
                if (!queued.open(QIODevice::WriteOnly) || queued.write("queued") != 6) {
                    app.exit(1);
                    return;
                }
            }
            if (!QFile::remove(result) ||
                !callbacks.install_apk(callbacks.user, QFile::encodeName(source).constData(), name.constData())) {
                app.exit(1);
                return;
            }
            stage = 3;
        } else if (stage == 3) {
            const QDir handoffs(QDir(root.path()).filePath(QStringLiteral("store-install-handoffs")));
            if (!status.contains("queue is full") ||
                handoffs.entryList(QStringList() << QStringLiteral("request_*.apk"), QDir::Files).size() != 8) {
                std::fprintf(stderr, "Queue limit was not enforced: %s\n", status.constData());
                app.exit(1);
                return;
            }
            for (int i = 0; i < 8; ++i) {
                if (!QFile::remove(handoffs.filePath(QStringLiteral("request_%1.apk").arg(i, 32, 16, QLatin1Char('0'))))) {
                    app.exit(1);
                    return;
                }
            }
            const QString sourceBundle = QDir(root.path()).filePath(QStringLiteral("source.apks"));
            QFile bundle(sourceBundle);
            if (!bundle.open(QIODevice::WriteOnly) || bundle.write("test bundle") != 11) {
                app.exit(1);
                return;
            }
            bundle.close();
            if (!callbacks.install_apk(callbacks.user, QFile::encodeName(sourceBundle).constData(), bundleName.constData())) {
                app.exit(1);
                return;
            }
            stage = 4;
            submitted = false;
        } else if (stage == 4 || stage == 5) {
            if (stage == 5 && !status.contains("does not match")) {
                std::fprintf(stderr, "Mismatched import fingerprint was not rejected\n");
                app.exit(1);
                return;
            }
            const QByteArray &next = stage == 4 ? tamperedName : declinedName;
            if (!callbacks.install_apk(callbacks.user, QFile::encodeName(source).constData(), next.constData())) {
                app.exit(1);
                return;
            }
            ++stage;
            submitted = false;
        } else if (stage == 6) {
            const QString sourceXapk = QDir(root.path()).filePath(QStringLiteral("source.xapk"));
            QFile xapk(sourceXapk);
            if (!xapk.open(QIODevice::WriteOnly) || xapk.write("test XAPK") != 9) {
                app.exit(1);
                return;
            }
            xapk.close();
            if (!callbacks.install_apk(callbacks.user, QFile::encodeName(sourceXapk).constData(), xapkName.constData())) {
                app.exit(1);
                return;
            }
            stage = 7;
            submitted = false;
        } else {
            poll.stop();
            auto *launch = new LaunchThread;
            launch->host = callbacks;
            QObject::connect(launch, &QThread::finished, &app, [&, launch]() {
                if (!launch->result || manager.launched != QLatin1String("org.example.library")) {
                    std::fprintf(stderr, "Verified app launch failed\n");
                    app.exit(1);
                    launch->deleteLater();
                    return;
                }
                launch->deleteLater();
                manager.launchResult = QStringLiteral("error:runtime unavailable");
                auto *failed = new LaunchThread;
                failed->host = callbacks;
                QObject::connect(failed, &QThread::finished, &app, [&, failed]() {
                    if (failed->result) {
                        std::fprintf(stderr, "Failed launch reply was accepted\n");
                        app.exit(1);
                        failed->deleteLater();
                        return;
                    }
                    failed->deleteLater();
                    manager.key = QString(257, QLatin1Char('x'));
                    manager.launchResult = QStringLiteral("ok");
                auto *untrusted = new LaunchThread;
                untrusted->host = callbacks;
                QObject::connect(untrusted, &QThread::finished, &app, [&, untrusted]() {
                    if (untrusted->result) {
                        std::fprintf(stderr, "Launch reply with oversized identity key was accepted\n");
                        app.exit(1);
                    } else {
                        manager.key = QStringLiteral("test-key");
                        manager.apps = QStringLiteral("MEGLER_INSTALL_STATUS_V1\n");
                        QTimer::singleShot(6000, &app, [&]() {
                            QFile marker(QDir(exporter.appsDir()).filePath(QStringLiteral("apps.txt")));
                            if (!marker.open(QIODevice::ReadOnly) ||
                                marker.readAll() != "MEGLER_INSTALL_STATUS_V1\n") {
                                std::fprintf(stderr, "Library removal did not refresh\n");
                                app.exit(1);
                                return;
                            }
                            manager.apps = QStringLiteral("invalid");
                            QTimer::singleShot(6000, &app, [&]() {
                                QFile unchanged(QDir(exporter.appsDir()).filePath(QStringLiteral("apps.txt")));
                                if (!unchanged.open(QIODevice::ReadOnly) ||
                                    unchanged.readAll() != "MEGLER_INSTALL_STATUS_V1\n") {
                                    std::fprintf(stderr, "Invalid list replaced cached library\n");
                                    app.exit(1);
                                    return;
                                }
                                manager.apps = QStringLiteral("MEGLER_INSTALL_STATUS_V1\norg.example.new\t\tNew\t1\t8\n");
                                QTimer::singleShot(6000, &app, [&]() {
                                    QFile current(QDir(exporter.appsDir()).filePath(QStringLiteral("apps.txt")));
                                    if (!current.open(QIODevice::ReadOnly) ||
                                        !current.readAll().contains("org.example.new\t")) {
                                        std::fprintf(stderr, "Library addition did not refresh\n");
                                        app.exit(1);
                                        return;
                                    }
                                    manager.trusted = false;
                                    StandaloneApkExport cached(root.path(), root.path(), other, nullptr, cachePath);
                                    QFile restored(QDir(cached.appsDir()).filePath(QStringLiteral("apps.txt")));
                                    if (!cached.error().isEmpty() || !restored.open(QIODevice::ReadOnly) ||
                                        !restored.readAll().contains("org.example.new\t")) {
                                        std::fprintf(stderr, "Library cache was not restored\n");
                                        app.exit(1);
                                        return;
                                    }
                                    app.quit();
                                });
                            });
                        });
                    }
                    untrusted->deleteLater();
                });
                untrusted->start();
                });
                failed->start();
            });
            launch->start();
        }
    });
    poll.start(10);
    QTimer::singleShot(45000, &app, [&]() { std::fprintf(stderr, "Handoff test timed out at stage %d\n", stage); app.exit(1); });
    const int resultCode = app.exec();
    if (!resultCode) std::puts("Standalone APK handoff OK");
    return resultCode;
}

#include "standalone_apk_export_test.moc"
