#include "../probe.h"
#include "../../core/src/apk/mg_apk.h"

#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
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
extern "C" char *megler_apk_report(const char *) { ++reportCalls; return nullptr; }
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
    const bool microgLifecycleTest = argc == 2 && QByteArray(argv[1]) == "--microg-lifecycle-test";
    const bool reminderTest = argc == 2 && QByteArray(argv[1]) == "--signature-reminder-test";
    if (argc == 1 || rpmGuardTest || rpmProgressTest || microgLifecycleTest || reminderTest)
        qputenv("XDG_DATA_HOME", QFile::encodeName(temporary.path()));
    if (reminderTest)
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(temporary.path()));
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName(QStringLiteral("org.test"));
    app.setApplicationName(QStringLiteral("multitask"));
    if (argc == 3 && QByteArray(argv[1]) == "--build-standalone") {
        std::fputs("MEGLER_RPM_STAGE:in", stderr);
        std::fflush(stderr);
        QThread::msleep(40);
        std::fputs("spect\n", stderr);
        std::fflush(stderr);
        for (const char *stage : {"build", "sign", "save"}) {
            QThread::msleep(40);
            std::fprintf(stderr, "MEGLER_RPM_STAGE:%s\n", stage);
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
    if (argc == 8 && QByteArray(argv[1]) == "--preview") {
        const QString data = QString::fromLocal8Bit(argv[3]);
        check(QDir().mkpath(data), "create preview data directory");
        writeFile(data + QStringLiteral("/started"), QByteArray(argv[4]));
        QTimer::singleShot(300, &app, &QCoreApplication::quit);
        return app.exec();
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
    emit probe.installedAppsChanged();
    probe.installedApps();
    check(reportCalls > firstReadCalls, "library cache invalidated on change");
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
    emit probe.installedAppsChanged();
    QCoreApplication::processEvents();
    check(QSettings().value(QStringLiteral("rpm/alwaysInstall")).toBool(),
          "legacy preference preserved but ignored");
    check(!QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/shortcut-rpm")).exists(), "no shortcut RPM directory created");
    if (rpmGuardTest) return 0;
    if (rpmProgressTest) {
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
        timeout.start(10000);
        wait.exec();
        check(timeout.isActive(), "RPM progress test timed out");
        check(stages.contains(QStringLiteral("Inspecting the app and preparing the package...")) &&
              stages.contains(QStringLiteral("Building the RPM with the app and runtime...")) &&
              stages.contains(QStringLiteral("Signing the RPM...")) &&
              stages.contains(QStringLiteral("Saving the RPM in the library...")),
              "all RPM stages reported");
        check(probe.standaloneRpmStatus().contains(QStringLiteral("test build failure")) &&
              !probe.standaloneRpmOpened(), "builder diagnostic preserved on failure");
        check(errorCount == 1 && errorPackage == QStringLiteral("org.example.runner1") &&
              errorMessage == probe.standaloneRpmStatus(), "RPM error emitted once with the package and diagnostic");
        probe.installStandaloneRpm(QStringLiteral("org.example.missing"));
        check(errorCount == 2 && errorPackage == QStringLiteral("org.example.missing") &&
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
    bool acceptStore = false;
    bool storeUpdatePrompt = false;
    int promptCount = 0;
    QObject::connect(&probe, &Probe::installConfirmationChanged, &app, [&]() {
        if (!probe.installConfirmationVisible()) return;
        ++promptCount;
        const QString package = probe.installConfirmationPackage();
        const QString token = probe.installConfirmationToken();
        if (package == QLatin1String("com.aurora.store")) {
            storeUpdatePrompt = probe.installConfirmationIsUpdate();
            QTimer::singleShot(0, &probe, [&probe, token, &acceptStore]() {
                probe.answerInstall(token, acceptStore);
            });
            return;
        }
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
    check(view.isVisible(), "controller stays accessible while microG runs");
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
    const QString storeInput = temporary.path() + QStringLiteral("/store.apk");
    const QString storeApk = store + QStringLiteral("/com.aurora.store/base.apk");
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
    };
    acceptStore = true;
    updateStore();
    check(QFile::exists(storeApk) && promptCount == 0, "local store added without confirmation");
    probe.openOfficialStore();
    check(!probe.previewRunning() && probe.officialStoreStatus().contains(QStringLiteral("standalone RPM")),
          "installed store opens only through RPM");
    const QString storeData = store + QStringLiteral("/com.aurora.store/data");
    check(QDir().mkpath(storeData), "create store data");
    writeFile(storeData + QStringLiteral("/saved"), "saved");
    writeFile(storeInput, "store2");
    acceptStore = false;
    updateStore();
    check(promptCount == 1 && storeUpdatePrompt && QFileInfo(storeApk).size() == 5, "declined store update preserves APK");
    acceptStore = true;
    updateStore();
    check(promptCount == 2 && storeUpdatePrompt && QFileInfo(storeApk).size() == 6 &&
          QFile::exists(storeData + QStringLiteral("/saved")), "store update preserves data");
    writeFile(storeInput, "external");
    updateStore();
    check(QFileInfo(storeApk).size() == 6 && !probe.officialStoreStatus().isEmpty(),
          "wrong package is rejected");
    view.close();
    check(!view.isVisible(), "controller closes after guests exit");
    std::puts("Probe RPM-only launch and import OK");
    return 0;
}
