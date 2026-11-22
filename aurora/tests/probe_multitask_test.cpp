#include "../probe.h"
#include "../../core/src/apk/mg_apk.h"

#include <QEventLoop>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool ok, const char *description)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static QStringList rpmEvents;
static void captureRpmEvents(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(QLatin1String("Standalone RPM: pid="))) rpmEvents.append(message);
}

class RpmUrlReceiver : public QObject
{
    Q_OBJECT
public:
    QUrl received;
public slots:
    void open(const QUrl &url) { received = url; }
};

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    std::memset(info, 0, sizeof(*info));
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray input = file.readAll();
    const char *package = input == "external" ? "org.install.external" :
                          input == "external1" ? "org.install.external1" :
                          input == "external2" ? "org.install.external2" :
                          input == "external3" ? "org.install.external3" :
                          input == "external4" ? "org.install.external4" :
                          input == "microg" ? "com.google.android.gms" :
                          input == "store" || input == "store2" ? "com.aurora.store" : nullptr;
    if (!package) return false;
    std::strcpy(info->package, package);
    std::strcpy(info->label, "Example");
    info->runnable = true;
    return true;
}
extern "C" bool mg_apk_is_microg(const char *path)
{
    QFile file(QString::fromUtf8(path));
    return file.open(QIODevice::ReadOnly) && file.readAll() == "microg";
}
extern "C" bool mg_apk_gms_name(const char *package)
{
    return std::strcmp(package, "com.google.android.gms") == 0;
}

static int reportCalls = 0;
static bool libraryReportTest = false;
extern "C" char *megler_apk_report(const char *path)
{
    ++reportCalls;
    if (!libraryReportTest) return nullptr;
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray contents = file.readAll();
    const char *package = contents == "external" ? "org.install.external" :
                          contents == "external1" ? "org.install.external1" :
                          contents == "external2" ? "org.install.external2" : nullptr;
    if (!package) return nullptr;
    const QByteArray json = QByteArray("{\"ok\":true,\"runnable\":true,\"package\":\"") + package +
                            "\",\"label\":\"Example\"}";
    return ::strdup(json.constData());
}
extern "C" bool megler_apk_icon(const char *, uint8_t **, size_t *) { return false; }
extern "C" char *megler_apk_report_splits(const char *, const char *) { return nullptr; }
extern "C" bool megler_apk_icon_splits(const char *, const char *, uint8_t **, size_t *) { return false; }
extern "C" void megler_free(void *p) { std::free(p); }

static void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "open input");
    check(file.write(contents) == contents.size(), "write input");
}

int main(int argc, char **argv)
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary root");
    const bool rpmGuardTest = argc == 2 && QByteArray(argv[1]) == "--rpm-guard-test";
    const bool rpmProgressTest = argc == 2 && QByteArray(argv[1]) == "--rpm-progress-test";
    const bool rpmCacheInstallerTest = argc == 2 && QByteArray(argv[1]) == "--rpm-cache-installer-test";
    const bool microgLifecycleTest = argc == 2 && QByteArray(argv[1]) == "--microg-lifecycle-test";
    const bool reminderTest = argc == 2 && QByteArray(argv[1]) == "--signature-reminder-test";
    const bool libraryRefreshTest = argc == 2 && QByteArray(argv[1]) == "--library-refresh-test";
    if (argc == 1 || rpmGuardTest || rpmProgressTest || rpmCacheInstallerTest || microgLifecycleTest || reminderTest || libraryRefreshTest)
        qputenv("XDG_DATA_HOME", QFile::encodeName(temporary.path()));
    if (argc == 1 || reminderTest || libraryRefreshTest)
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(temporary.path()));
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName(QStringLiteral("org.test"));
    app.setApplicationName(QStringLiteral("multitask"));
    if ((argc == 3 || argc == 4) && QByteArray(argv[1]) == "--build-standalone") {
        if (argc != 3) return 2;
        const QString cachedPath = QString::fromLocal8Bit(qgetenv("ACL_TEST_CACHED_RPM"));
        if (!cachedPath.isEmpty()) {
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("path"), cachedPath);
            std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            return 0;
        }
        std::fputs("ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:in", stderr);
        std::fflush(stderr);
        QThread::msleep(40);
        std::fputs("spect\n", stderr);
        std::fflush(stderr);
        for (const char *stage : {"build", "sign", "save"}) {
            QThread::msleep(40);
            std::fprintf(stderr, "ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:%s\n", stage);
            std::fflush(stderr);
        }
        QThread::msleep(40);
        std::fputs("test build failure", stderr);
        std::puts("{\"ok\":false}");
        return 1;
    }
    if (argc == 10 && QByteArray(argv[1]) == "--unpack-bundle") {
        writeFile(QString::fromLocal8Bit(argv[6]), "external");
        check(QDir().mkpath(QString::fromLocal8Bit(argv[7])), "create mock splits");
        std::puts("{\"ok\":true,\"package\":\"org.install.bundle\",\"versionCode\":42}");
        return 0;
    }
    if (argc == 10 && (QByteArray(argv[1]) == "--unpack-store-bundle" ||
                       QByteArray(argv[1]) == "--unpack-store-xapk")) {
        QFile input(QString::fromLocal8Bit(argv[4]));
        check(input.open(QIODevice::ReadOnly), "open store bundle snapshot");
        writeFile(QString::fromLocal8Bit(argv[6]), input.readAll());
        check(QDir().mkpath(QString::fromLocal8Bit(argv[7])), "create mock store splits");
        std::puts(QByteArray(argv[1]) == "--unpack-store-xapk"
            ? "{\"ok\":true,\"package\":\"org.install.storexapk\",\"versionCode\":42}"
            : "{\"ok\":true,\"package\":\"org.install.storebundle\",\"versionCode\":42}");
        return 0;
    }
    if (argc == 8 && QByteArray(argv[1]) == "--preview") {
        const QString data = QString::fromLocal8Bit(argv[3]);
        check(QDir().mkpath(data), "create preview data directory");
        writeFile(data + QStringLiteral("/started"), QByteArray(argv[4]));
        QTimer::singleShot(300, &app, &QCoreApplication::quit);
        return app.exec();
    }

    if (libraryRefreshTest) {
        libraryReportTest = true;
        Probe manager(QStringLiteral("/runtime"));
        QStringList added;
        QStringList rpmErrors;
        QObject::connect(&manager, &Probe::libraryAppAddedNotice, &app,
                         [&](const QString &package) { added.append(package); });
        QObject::connect(&manager, &Probe::standaloneRpmError, &app,
                         [&](const QString &package, const QString &) { rpmErrors.append(package); });
        check(!manager.autoInstallRpm(), "automatic RPM installation defaults to off");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("probe"), &manager);
        QQmlComponent component(&engine);
        component.setData("import QtQuick 2.0\nQtObject { property var apps: probe.installedApps; property int count: apps.length }",
                          QUrl());
        QObject *binding = component.create();
        check(binding != nullptr, "create QML library binding");
        check(binding->property("count").toInt() == 0, "library initially empty");
        const QString apk = temporary.path() + QStringLiteral("/library.apk");
        writeFile(apk, "external");
        manager.installApk(apk);
        check(binding->property("count").toInt() == 1, "first import updates QML library");
        check(added == QStringList{QStringLiteral("org.install.external")} && !manager.standaloneRpmBusy(),
              "successful import emits a notice without automatic RPM by default");
        const int firstReadCalls = reportCalls;
        manager.installedApps();
        manager.installedApps();
        check(reportCalls == firstReadCalls, "library metadata is cached after refresh");
        writeFile(apk, "external1");
        manager.installApk(apk);
        check(binding->property("count").toInt() == 2, "second import updates QML library");
        manager.installApk(apk);
        check(added.size() == 2, "failed duplicate import emits no notice");
        check(manager.removeApp(QStringLiteral("org.install.external")), "remove first app");
        check(binding->property("count").toInt() == 1, "removal updates QML library");
        writeFile(apk, "external");
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&manager, &Probe::externalInstallBusyChanged, &wait, [&]() {
            if (!manager.externalInstallBusy()) wait.quit();
        });
        manager.requestExternalInstall(apk);
        check(manager.externalInstallBusy(), "external import started");
        timeout.start(10000);
        wait.exec();
        check(timeout.isActive(), "external import timed out");
        check(binding->property("count").toInt() == 2, "external import updates QML library");
        check(added.last() == QStringLiteral("org.install.external"), "external import reports its package");
        manager.setAutoInstallRpm(true);
        check(manager.autoInstallRpm(), "automatic RPM setting saved");
        {
            Probe reloaded(QStringLiteral("/runtime"));
            check(reloaded.autoInstallRpm(), "automatic RPM setting persists across instances");
        }
        writeFile(apk, "external2");
        manager.installApk(apk);
        check(added.last() == QStringLiteral("org.install.external2"), "direct import reports its package");
        manager.installApk(apk);
        check(added.size() == 4, "failed direct import does not queue RPM");
        writeFile(apk, "external3");
        manager.requestExternalInstall(apk);
        if (manager.externalInstallBusy()) {
            timeout.start(10000);
            wait.exec();
            check(timeout.isActive(), "automatic external import timed out");
        }
        check(added.last() == QStringLiteral("org.install.external3"), "automatic external import reports its package");
        const QString bundle = temporary.path() + QStringLiteral("/test.xapk");
        writeFile(bundle, "bundle");
        manager.installApk(bundle);
        if (manager.externalInstallBusy()) {
            timeout.start(10000);
            wait.exec();
            check(timeout.isActive(), "bundle import timed out");
        }
        check(added.last() == QStringLiteral("org.install.bundle"), "bundle import reports its package");
        writeFile(apk, "external4");
        bool handoffFinished = false;
        {
            QEventLoop handoffWait;
            QTimer handoffTimeout;
            handoffTimeout.setSingleShot(true);
            QObject::connect(&handoffTimeout, &QTimer::timeout, &handoffWait, &QEventLoop::quit);
            QObject::connect(&manager, &Probe::storeHandoffFinished, &handoffWait,
                             [&](bool retry, bool installed, const QByteArray &fingerprint) {
                check(!retry && installed &&
                      fingerprint == QCryptographicHash::hash(QByteArray("external4"), QCryptographicHash::Sha256).toHex(),
                      "headless store import reports the actual snapshot");
                handoffFinished = true;
                handoffWait.quit();
            });
            QString error;
            check(manager.requestStoreHandoff(apk, &error), "start headless automatic store import");
            handoffTimeout.start(10000);
            handoffWait.exec();
            check(handoffFinished && handoffTimeout.isActive(), "headless store import completed");
        }
        check(added.last() == QStringLiteral("org.install.external4"),
              "headless store import queues automatic RPM");
        if (rpmErrors.size() < 4) {
            QEventLoop rpmWait;
            QTimer rpmTimeout;
            rpmTimeout.setSingleShot(true);
            QObject::connect(&rpmTimeout, &QTimer::timeout, &rpmWait, &QEventLoop::quit);
            QObject::connect(&manager, &Probe::standaloneRpmError, &rpmWait,
                             [&]() { if (rpmErrors.size() >= 4) rpmWait.quit(); });
            rpmTimeout.start(10000);
            rpmWait.exec();
            check(rpmTimeout.isActive(), "automatic RPM queue timed out");
        }
        check(rpmErrors == (QStringList() << QStringLiteral("org.install.external2")
                                         << QStringLiteral("org.install.external3")
                                         << QStringLiteral("org.install.bundle")
                                         << QStringLiteral("org.install.external4")),
              "automatic RPM queue attempts each successful import and reports builder failures");
        manager.setAutoInstallRpm(false);
        check(!manager.autoInstallRpm(), "automatic RPM can be disabled");
        delete binding;
        return 0;
    }

    const QString store = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/apps");
    const QString first = store + QStringLiteral("/org.example.runner1");
    check(QDir().mkpath(first + QStringLiteral("/data")), "create app root");
    writeFile(first + QStringLiteral("/base.apk"), "placeholder");
    const QString external = temporary.path() + QStringLiteral("/external.apk");
    writeFile(external, "external");

    QSettings().setValue(QStringLiteral("rpm/alwaysInstall"), true);
    if (reminderTest) {
        int reminders = 0;
        auto import = [&](Probe &manager, const QString &path) {
            QEventLoop wait;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
            QObject::connect(&manager, &Probe::externalInstallBusyChanged, &wait, [&]() {
                if (!manager.externalInstallBusy()) wait.quit();
            });
            manager.requestExternalInstall(path);
            if (manager.externalInstallBusy()) {
                timeout.start(10000);
                wait.exec();
                check(timeout.isActive(), "signature reminder import timed out");
            }
        };
        {
            Probe firstProbe(QStringLiteral("/runtime"));
            QObject::connect(&firstProbe, &Probe::signatureReminder, &app, [&]() { ++reminders; });
            import(firstProbe, temporary.path() + QStringLiteral("/missing.apk"));
            check(reminders == 0, "failed imports do not show reminders");
            import(firstProbe, external);
            writeFile(external, "external1");
            import(firstProbe, external);
            check(reminders == 2, "first two imports show reminders");
        }
        {
            Probe secondProbe(QStringLiteral("/runtime"));
            QObject::connect(&secondProbe, &Probe::signatureReminder, &app, [&]() { ++reminders; });
            writeFile(external, "external2");
            import(secondProbe, external);
            writeFile(external, "external3");
            import(secondProbe, external);
            check(reminders == 3, "reminder limit persists across controller restarts");
        }
        return 0;
    }
    Probe probe(QStringLiteral("/runtime"));
    probe.installedApps();
    const int firstReadCalls = reportCalls;
    probe.installedApps();
    check(firstReadCalls > 0 && reportCalls == firstReadCalls, "library metadata is cached");
    const QVariantMap permissionApp{
        {QStringLiteral("package"), QStringLiteral("org.example.runner1")},
        {QStringLiteral("permissions"), QVariantList{QStringLiteral("android.permission.CAMERA")}},
        {QStringLiteral("permissionMaxSdk"), QVariantList{0}}
    };
    QVariantList permissionGroups = probe.appPermissionGroups(QStringLiteral("org.example.runner1"), permissionApp);
    check(permissionGroups.size() == 1 && permissionGroups.first().toMap().value(QStringLiteral("allowed")).toBool(),
          "permissions use selected app metadata without rescanning the library");
    check(probe.setAppPermission(QStringLiteral("org.example.runner1"), QStringLiteral("camera"), false),
          "save app permission");
    permissionGroups = probe.appPermissionGroups(QStringLiteral("org.example.runner1"), permissionApp);
    check(permissionGroups.size() == 1 && !permissionGroups.first().toMap().value(QStringLiteral("allowed")).toBool(),
          "saved permission is reloaded");
    check(probe.appPermissionGroups(QStringLiteral("org.example.runner1"), QVariantMap()).isEmpty() &&
          !probe.appResult().isEmpty(), "missing app metadata reports an error");
    check(probe.metaObject()->indexOfProperty("alwaysInstallRpm") < 0 &&
          probe.metaObject()->indexOfMethod("setAlwaysInstallRpm(bool)") < 0 &&
          probe.metaObject()->indexOfMethod("installAsRpm(QString)") < 0,
          "obsolete automatic RPM interface removed");
    QCoreApplication::processEvents();
    check(QSettings().value(QStringLiteral("rpm/alwaysInstall")).toBool(),
          "legacy preference preserved but ignored");
    check(!QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/shortcut-rpm")).exists(), "no shortcut RPM directory created");
    if (rpmGuardTest) return 0;
    if (rpmCacheInstallerTest) {
        const QString cacheDir = first + QStringLiteral("/standalone-rpm");
        check(QDir().mkpath(cacheDir), "create RPM cache");
        const QString cached = cacheDir + QStringLiteral("/cached.rpm");
        writeFile(cached, "cached package");
        qputenv("ACL_TEST_CACHED_RPM", QFile::encodeName(cached));
        RpmUrlReceiver receiver;
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &receiver, "open");
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::standaloneRpmChanged, &wait, [&]() {
            if (!probe.standaloneRpmBusy()) wait.quit();
        });
        probe.installStandaloneRpm(QStringLiteral("org.example.runner1"));
        timeout.start(10000);
        wait.exec();
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        qunsetenv("ACL_TEST_CACHED_RPM");
        check(timeout.isActive() && probe.standaloneRpmOpened() &&
              receiver.received == QUrl::fromLocalFile(cached), "cached RPM reaches installer handler");
        return 0;
    }
    if (rpmProgressTest) {
        QtMessageHandler previousHandler = qInstallMessageHandler(captureRpmEvents);
        QSet<QString> stages;
        int errorCount = 0;
        QString errorPackage;
        QString errorMessage;
        QObject::connect(&probe, &Probe::standaloneRpmError, &app, [&](const QString &package, const QString &message) {
            ++errorCount;
            errorPackage = package;
            errorMessage = message;
        });
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::standaloneRpmChanged, &wait, [&]() {
            stages.insert(probe.standaloneRpmStatus());
            if (!probe.standaloneRpmBusy()) wait.quit();
        });
        probe.installStandaloneRpm(QStringLiteral("org.example.runner1"));
        check(probe.standaloneRpmBusy(), "RPM builder started");
        probe.installStandaloneRpm(QStringLiteral("org.example.other-private-package"));
        timeout.start(10000);
        wait.exec();
        qInstallMessageHandler(previousHandler);
        check(timeout.isActive(), "RPM progress test timed out");
        const QString prefix = QStringLiteral("Standalone RPM: pid=%1 request=1 event=")
                                   .arg(QCoreApplication::applicationPid());
        check(rpmEvents.size() == 8 &&
              rpmEvents.at(0) == prefix + QStringLiteral("accepted") &&
              rpmEvents.at(1) == prefix + QStringLiteral("ignored-busy") &&
              rpmEvents.at(2).startsWith(prefix + QStringLiteral("started worker=")) &&
              rpmEvents.at(3) == prefix + QStringLiteral("stage-inspect") &&
              rpmEvents.at(4) == prefix + QStringLiteral("stage-build") &&
              rpmEvents.at(5) == prefix + QStringLiteral("stage-sign") &&
              rpmEvents.at(6) == prefix + QStringLiteral("stage-save") &&
              rpmEvents.at(7) == prefix + QStringLiteral("finished exit=1 normal=1 timed-out=0"),
              "RPM lifecycle events correlate stages, ignored requests and completion without package data");
        check(stages.contains(QStringLiteral("Inspecting the app and preparing the package...")) &&
              stages.contains(QStringLiteral("Building the RPM with the app and runtime...")) &&
              stages.contains(QStringLiteral("Signing the RPM...")) &&
              stages.contains(QStringLiteral("Saving the RPM in the library...")),
              "all RPM stages reported");
        check(probe.standaloneRpmStatus().contains(QStringLiteral("test build failure")) &&
              !probe.standaloneRpmOpened(), "builder diagnostic preserved on failure");
        check(errorCount == 1 && errorPackage == QStringLiteral("org.example.runner1") &&
              errorMessage == probe.standaloneRpmStatus(), "RPM error emitted once with the package and diagnostic");
        check(probe.standaloneRpmStatus().contains(QStringLiteral("test build failure")),
              "RPM generation is independent of detailed logging state");
        probe.setDetailedLogEnabled(true);
        probe.installStandaloneRpm(QStringLiteral("org.example.runner1"));
        check(probe.standaloneRpmBusy(), "diagnostic RPM builder started");
        timeout.start(10000);
        wait.exec();
        check(timeout.isActive() &&
              probe.standaloneRpmStatus().contains(QStringLiteral("test build failure")),
              "RPM generation uses the same snapshot configuration while logging is enabled");
        probe.setDetailedLogEnabled(false);
        probe.installStandaloneRpm(QStringLiteral("org.example.missing"));
        check(errorCount == 3 && errorPackage == QStringLiteral("org.example.missing") &&
              errorMessage == probe.standaloneRpmStatus(), "missing APK emits an RPM error");
        return 0;
    }

    QQuickView view;
    view.setSource(QUrl(QStringLiteral("data:text/plain,import QtQuick 2.0%0AItem %7B%7D")));
    check(view.status() == QQuickView::Ready, "controller view ready");
    probe.setConfirmationWindow(&view);
    probe.setInstallUiReady(true);
    view.show();
    view.requestActivate();
    QCoreApplication::processEvents();
    check(view.isActive(), "controller active");
    const QString storeBundle = temporary.path() + QStringLiteral("/handoff.apks");
    int promptCount = 0;
    QObject::connect(&probe, &Probe::installConfirmationChanged, &app, [&]() {
        if (!probe.installConfirmationVisible()) return;
        ++promptCount;
        check(false, "unexpected library import confirmation");
    });
    probe.runInstalled(QStringLiteral("org.example.runner1"));
    check(!probe.previewRunning() && !probe.isAppRunning(QStringLiteral("org.example.runner1")) &&
          probe.appResult().contains(QStringLiteral("standalone RPM")), "ordinary app launch refused");

    const QString microg = temporary.path() + QStringLiteral("/microg.apk");
    const QString microgData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/services/data");
    writeFile(microg, "microg");
    probe.setGoogleServicesGmsApkPath(microg);
    int libraryChanges = 0;
    QObject::connect(&probe, &Probe::installedAppsChanged, &probe, [&libraryChanges]() { ++libraryChanges; });
    probe.openMicrog();
    check(probe.previewRunning() && probe.isAppRunning(QStringLiteral("__microg__")),
          "microG settings launch remains available");
    view.close();
    check(!view.isVisible() && probe.previewRunning(), "controller closes without stopping microG");
    view.show();
    check(view.isVisible(), "controller can be reopened while microG runs");
    QEventLoop microgLoop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &microgLoop, [&]() {
        if (!probe.previewRunning()) microgLoop.quit();
    });
    poll.start(20);
    QTimer::singleShot(15000, &microgLoop, &QEventLoop::quit);
    microgLoop.exec();
    check(!probe.previewRunning(), "microG preview finished");
    check(libraryChanges == 0, "microG lifecycle does not refresh unchanged APK library");
    QFile marker(microgData + QStringLiteral("/started"));
    check(marker.open(QIODevice::ReadOnly) && marker.readAll() == QFile::encodeName(microg),
          "microG preview received the selected APK");
    if (microgLifecycleTest) return 0;

    auto importExternal = [&]() {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::externalInstallBusyChanged, &wait, [&]() {
            if (!probe.externalInstallBusy()) wait.quit();
        });
        probe.requestExternalInstall(external);
        if (probe.externalInstallBusy()) {
            timeout.start(15000);
            wait.exec();
            check(timeout.isActive(), "external import timed out");
        }
    };
    importExternal();
    if (!QFile::exists(store + QStringLiteral("/org.install.external/base.apk")))
        std::fprintf(stderr, "External import: %s\n", qPrintable(probe.result()));
    check(promptCount == 0 &&
          QFile::exists(store + QStringLiteral("/org.install.external/base.apk")),
          "external APK added without a confirmation");
    importExternal();
    check(promptCount == 0 && QFileInfo(store + QStringLiteral("/org.install.external/base.apk")).size() == 8,
          "duplicate external APK does not replace the library entry");
    const QString bundle = temporary.path() + QStringLiteral("/bundle.xapk");
    writeFile(bundle, "bundle");
    const QString bundleApk = store + QStringLiteral("/org.install.bundle/base.apk");
    auto importBundle = [&]() {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::externalInstallBusyChanged, &wait, [&]() {
            if (!probe.externalInstallBusy()) wait.quit();
        });
        probe.requestExternalInstall(bundle);
        if (probe.externalInstallBusy()) {
            timeout.start(15000);
            wait.exec();
            check(timeout.isActive(), "bundle import timed out");
        }
    };
    importBundle();
    check(promptCount == 0 && QFile::exists(bundleApk),
          "external bundle added without a confirmation");
    importBundle();
    check(promptCount == 0 && QFile::exists(bundleApk),
          "duplicate bundle does not replace the library entry");
    const QString handoffApk = store + QStringLiteral("/org.install.storebundle/base.apk");
    auto importStoreBundle = [&](bool expectedSuccess) {
        writeFile(storeBundle, "original bundle");
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool completed = false;
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::storeHandoffFinished, &wait,
                         [&](bool retry, bool installed, const QByteArray &fingerprint) {
            check(!retry, "store bundle does not retry");
            check(installed == expectedSuccess, "store bundle status reflects import");
            const QByteArray expected = expectedSuccess
                ? QCryptographicHash::hash(QByteArray("original bundle"), QCryptographicHash::Sha256).toHex()
                : QByteArray();
            check(fingerprint == expected, "store bundle status identifies the immutable snapshot");
            completed = true;
            wait.quit();
        });
        QString error;
        check(probe.requestStoreHandoff(storeBundle, &error), "start store bundle import");
        timeout.start(15000);
        wait.exec();
        check(completed && timeout.isActive(), "store bundle import finished");
    };
    probe.setInstallUiReady(false);
    importStoreBundle(true);
    QFile importedHandoff(handoffApk);
    check(promptCount == 0 && importedHandoff.open(QIODevice::ReadOnly) &&
          importedHandoff.readAll() == "original bundle", "store bundle imported without confirmation UI");
    check(!QFile::exists(store + QStringLiteral("/org.install.storebundle/.store-source.apks")),
          "temporary store bundle is not retained in the library");
    importStoreBundle(false);
    check(promptCount == 0, "duplicate store bundle fails without a confirmation");
    const QString storeXapk = temporary.path() + QStringLiteral("/handoff.xapk");
    const QString handoffXapk = store + QStringLiteral("/org.install.storexapk/base.apk");
    writeFile(storeXapk, "original XAPK");
    {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool completed = false;
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::storeHandoffFinished, &wait,
                         [&](bool retry, bool installed, const QByteArray &fingerprint) {
            check(!retry && installed, "store XAPK import succeeds without confirmation UI");
            check(fingerprint == QCryptographicHash::hash(QByteArray("original XAPK"), QCryptographicHash::Sha256).toHex(),
                  "store XAPK result identifies the immutable snapshot");
            completed = true;
            wait.quit();
        });
        QString error;
        check(probe.requestStoreHandoff(storeXapk, &error), "start store XAPK import");
        timeout.start(15000);
        wait.exec();
        QFile importedXapk(handoffXapk);
        check(completed && timeout.isActive() && importedXapk.open(QIODevice::ReadOnly) &&
              importedXapk.readAll() == "original XAPK", "store XAPK reaches the bundle importer");
        check(!QFile::exists(store + QStringLiteral("/org.install.storexapk/.store-source.xapk")),
              "temporary store XAPK is not retained in the library");
    }
    const QString storeSingle = temporary.path() + QStringLiteral("/handoff.apk");
    writeFile(storeSingle, "external3");
    {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool completed = false;
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::storeHandoffFinished, &wait,
                         [&](bool retry, bool installed, const QByteArray &fingerprint) {
            check(!retry && installed, "store APK import succeeds without confirmation UI");
            check(fingerprint == QCryptographicHash::hash(QByteArray("external3"), QCryptographicHash::Sha256).toHex(),
                  "store APK result identifies the imported snapshot");
            completed = true;
            wait.quit();
        });
        QString error;
        check(probe.requestStoreHandoff(storeSingle, &error), "start store APK import");
        timeout.start(15000);
        wait.exec();
        check(completed && timeout.isActive() && promptCount == 0 &&
              QFile::exists(store + QStringLiteral("/org.install.external3/base.apk")),
              "store APK added without confirmation");
    }
    const QString storeInput = temporary.path() + QStringLiteral("/store.apk");
    const QString storeApk = store + QStringLiteral("/com.aurora.store/base.apk");
    probe.setAutoInstallRpm(true);
    check(probe.autoInstallRpm(), "automatic RPM is enabled while updating Aurora Store");
    bool storeRpmRequested = false;
    QObject::connect(&probe, &Probe::standaloneRpmChanged, &app, [&]() {
        if (probe.standaloneRpmPackage() == QLatin1String("com.aurora.store")) storeRpmRequested = true;
    });
    writeFile(storeInput, "store");
    auto updateStore = [&]() {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::officialStoreChanged, &wait, [&]() {
            if (!probe.officialStoreBusy()) wait.quit();
        });
        probe.updateOfficialStoreFromLocal(storeInput);
        if (probe.officialStoreBusy()) {
            timeout.start(15000);
            wait.exec();
            check(timeout.isActive(), "store update timed out");
        }
        QCoreApplication::processEvents();
        check(!storeRpmRequested, "Aurora Store settings imports do not request a standalone RPM");
    };
    updateStore();
    check(QFile::exists(storeApk) && promptCount == 0, "local store added without confirmation");
    probe.openOfficialStore();
    check(probe.previewRunning() && probe.isAppRunning(QStringLiteral("com.aurora.store")),
          "installed store starts in a separate preview process");
    const QString storeData = store + QStringLiteral("/com.aurora.store/data");
    {
        QEventLoop wait;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
        QObject::connect(&probe, &Probe::previewRunningChanged, &wait, [&]() {
            if (!probe.previewRunning()) wait.quit();
        });
        timeout.start(15000);
        wait.exec();
        check(timeout.isActive() && QFile::exists(storeData + QStringLiteral("/started")),
              "store preview process receives the library data directory");
    }
    check(QDir().mkpath(storeData), "create store data");
    writeFile(storeData + QStringLiteral("/saved"), "saved");
    writeFile(storeInput, "store2");
    updateStore();
    check(promptCount == 0 && QFileInfo(storeApk).size() == 6 &&
          QFile::exists(storeData + QStringLiteral("/saved")), "store update preserves data without confirmation");
    writeFile(storeInput, "external");
    updateStore();
    check(QFileInfo(storeApk).size() == 6 && !probe.officialStoreStatus().isEmpty(),
          "wrong package is rejected");
    view.close();
    check(!view.isVisible(), "controller closes after guests exit");
    std::puts("Probe RPM-only launch and import OK");
    return 0;
}

#include "probe_multitask_test.moc"
