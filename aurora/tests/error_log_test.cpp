#include "../error_log.h"
#include "../diagnostic_log.h"
#include "../memory_log_bus.h"
#include "../../core/include/megler.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVirtualObject>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <sys/file.h>
#include <unistd.h>

static void check(bool value, const char *description)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static void coreLogSignalHandler(int)
{
    ErrorLog::sendCoreLine(MEGLER_LOG_ERROR, "core signal handler event");
}

static const char RemoteGpuSummary[] =
    "GPU Canvas performance: interval_ms=42 frames=1 failed=0 samples=1 producer_us=1 "
    "producer_max_us=1 qt_wait_us=1 qt_wait_max_us=1 readbacks=0 readback_samples=0 "
    "readback_us=0 readback_max_us=0";

static const char FullGpuSummary[] =
    "GPU Canvas performance: interval_ms=3000 frames=240 failed=2 samples=15 "
    "producer_us=123456 producer_max_us=24000 qt_wait_us=9876 qt_wait_max_us=4500 "
    "readbacks=12 readback_samples=1 readback_us=789 readback_max_us=789 "
    "readback_flush=3 readback_filter=4 readback_self=2 readback_gl_us=600 "
    "readback_convert_us=189 readback_present=3 "
    "pixel_submit_samples=1 pixel_submit_us=3000 pixel_submit_max_us=3000 "
    "filter_fallback_mode=1 filter_fallback_blend=2 filter_fallback_self=3 "
    "filter_fallback_gles=4 filter_fallback_invalid=5 filter_draw_errors=6 "
    "readback_cache_flush=0 readback_cache_filter=7 readback_cache_self=1 readback_cache_present=2";

class Identity : public QDBusVirtualObject
{
public:
    QString introspect(const QString &) const override { return QString(); }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.member() != QLatin1String("IdentifyByBusName")) return false;
        const QDBusReply<QString> manager = connection.interface()->serviceOwner(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
        const bool isManager = manager.isValid() && message.arguments().size() == 1 &&
            message.arguments().first().toString() == manager.value();
        connection.send(message.createReply(QVariantList()
            << (isManager ? QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.appmanager")
                          : QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.Android0123456789abcdef01234567"))
            << QStringLiteral("test-key")));
        return true;
    }
};

int main(int argc, char **argv)
{
    if (argc == 3 && (QByteArray(argv[1]) == "--standalone-snapshot" ||
                      QByteArray(argv[1]) == "--standalone-snapshot-stress" ||
                      QByteArray(argv[1]) == "--standalone-snapshot-overflow" ||
                      QByteArray(argv[1]) == "--standalone-snapshot-repeats")) {
        QCoreApplication app(argc, argv);
        QString error;
        const QString appId = QStringLiteral(
            "ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
        check(ErrorLog::initialize(QStringLiteral("standalone"), &error), "initialize snapshot-only client");
        check(ErrorLog::startStandaloneSnapshots(QString::fromLocal8Bit(argv[2]), appId, &error),
              "start shared standalone snapshots");
        check(ErrorLog::startForwarding(&error), "start State-only D-Bus client");
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !MemoryLogBus::clientDetailedEnabled()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        check(MemoryLogBus::clientDetailedEnabled(), "wait for confirmed logging State");
        if (QByteArray(argv[1]) == "--standalone-snapshot-repeats") {
            for (int i = 0; i < 2000; ++i) {
                ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("identical error"), true);
                if (i == 999)
                    check(ErrorLog::flushStandaloneSnapshots(), "publish repeated errors");
            }
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("identical error"), true);
            ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("different error"), true);
        } else if (QByteArray(argv[1]) != "--standalone-snapshot") {
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("startup retained"), false);
            const bool overflow = QByteArray(argv[1]) == "--standalone-snapshot-overflow";
            const int count = 7000;
            for (int i = 0; i < count; ++i) {
                ErrorLog::record(QStringLiteral("Android"),
                                 i == 100 ? QStringLiteral("important retained error")
                                          : QStringLiteral("verbose event %1").arg(i) +
                                            (overflow ? QString(1600, QLatin1Char('x')) : QString()),
                                 i == 100);
                if (i % 1000 == 999)
                    check(ErrorLog::flushStandaloneSnapshots(), "publish bounded snapshot batch");
            }
        } else {
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("snapshot-only private event"), false);
        }
        check(ErrorLog::flushStandaloneSnapshots(), "publish standalone snapshot");
        return 0;
    }
    if (argc == 4 && QByteArray(argv[1]) == "--standalone-snapshot-update") {
        QCoreApplication app(argc, argv);
        QString error;
        const QString appId = QStringLiteral(
            "ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
        check(ErrorLog::initialize(QStringLiteral("standalone"), &error), "initialize updating snapshot client");
        check(ErrorLog::startStandaloneSnapshots(QString::fromLocal8Bit(argv[2]), appId, &error),
              "start updating shared snapshot");
        check(ErrorLog::startForwarding(&error), "start State-only updating client");
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !MemoryLogBus::clientDetailedEnabled()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        check(MemoryLogBus::clientDetailedEnabled(), "wait for confirmed logging State");
        ErrorLog::record(QStringLiteral("Android"), QStringLiteral("snapshot-before-export"), false);
        check(ErrorLog::flushStandaloneSnapshots(), "publish pre-export snapshot");
        bool updated = false;
        QTimer pollTrigger;
        pollTrigger.setInterval(2);
        QObject::connect(&pollTrigger, &QTimer::timeout, &app, [&]() {
            if (!updated && QFileInfo::exists(QString::fromLocal8Bit(argv[3]))) {
                ErrorLog::record(QStringLiteral("Android"), QStringLiteral("snapshot-during-export"), false);
                updated = true;
            }
        });
        pollTrigger.start();
        QTimer publishTimer;
        publishTimer.setInterval(20);
        QObject::connect(&publishTimer, &QTimer::timeout, &app, []() {
            ErrorLog::flushStandaloneSnapshots();
        });
        publishTimer.start();
        QTimer::singleShot(2500, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (argc == 2 && QByteArray(argv[1]) == "--gpu-summary") {
        QCoreApplication app(argc, argv);
        QString error;
        check(ErrorLog::initialize(QStringLiteral("Android App Manager"), &error) &&
              ErrorLog::setDetailedEnabled(true, &error), "initialize local GPU diagnostic reporting");
        const QString old = QString::fromLatin1(FullGpuSummary);
        const QString current = old + QStringLiteral(" canvas_copies_gpu=3");
        ErrorLog::record(QStringLiteral("Qt"), current + QStringLiteral(" PRIVATE-GPU"), false);
        ErrorLog::record(QStringLiteral("Qt"), old + QStringLiteral(" canvas_copies_gpu="), false);
        check(!ErrorLog::report().contains(old), "malformed GPU summaries are not accepted");
        ErrorLog::record(QStringLiteral("Qt"), old, false);
        ErrorLog::record(QStringLiteral("Qt"), current, false);
        const QString report = ErrorLog::report();
        check(report.contains(old + QLatin1Char('\n')) && report.contains(current + QLatin1Char('\n')),
              "old and new GPU counters are visible in safe diagnostics");
        return 0;
    }
    if (argc == 3) {
        QCoreApplication app(argc, argv);
        const QByteArray mode(argv[1]);
        QString error, text;
        check(ErrorLog::initialize(QStringLiteral("Android App Manager"), &error), "initialize persisted manager");
        if (mode == "--corrupt" || mode == "--bad-root") {
            check(!ErrorLog::startPersistence(QString::fromLocal8Bit(argv[2]), &error) &&
                  !ErrorLog::persistenceError().isEmpty(), "reject corrupted snapshot");
            return 0;
        }
        check(ErrorLog::startPersistence(QString::fromLocal8Bit(argv[2]), &error), "start private storage");
        if (mode == "--core-quit") {
            check(ErrorLog::setDetailedEnabled(true, &error) && ErrorLog::startCoreChannel(&error),
                  "start persistent core channel");
            QTimer::singleShot(0, &app, [&app]() {
                ErrorLog::sendCoreLine(MEGLER_LOG_INFO, "core event immediately before quit");
                app.quit();
            });
            return app.exec();
        }
        if (mode == "--read-core-quit") {
            check(ErrorLog::detailedReport(&text, &error) &&
                  text.contains(QStringLiteral("core event immediately before quit")),
                  "core channel is drained before the final snapshot");
            return 0;
        }
        if (mode == "--fail-write") {
            check(QDir().mkpath(QString::fromLocal8Bit(argv[2]) +
                                QStringLiteral("/diagnostic-log/manager-state-v1.dat.pending")),
                  "block snapshot write");
            check(!ErrorLog::setDetailedEnabled(true, &error) && !error.isEmpty() &&
                  !ErrorLog::persistenceError().isEmpty(), "snapshot write failure is reported");
            bool actual = false;
            check(ErrorLog::detailedEnabled(&actual, &error) && actual, "failed save retains changed runtime state");
            return 0;
        }
        if (mode == "--write") {
            check(ErrorLog::setDetailedEnabled(true, &error), "enable persistent detailed logging");
            const QString large(65536, QLatin1Char('x'));
            for (int i = 0; i < 140; ++i)
                ErrorLog::record(QStringLiteral("test"), large, false);
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("SecurityException persistent private secret"), true);
            return app.exec();
        }
        bool enabled = false;
        check(ErrorLog::detailedEnabled(&enabled, &error), "read restored detailed state");
        if (mode == "--read") {
            check(enabled && ErrorLog::detailedReport(&text, &error) &&
                  text.contains(QStringLiteral("persistent private secret")), "restore detailed history");
            check(!text.contains(QStringLiteral("[Earlier detailed output was truncated.]")) &&
                  !text.contains(QStringLiteral("[Manager log truncated")) &&
                  text.toUtf8().size() < 10000000,
                  "detailed history below ten megabytes is preserved");
            check(ErrorLog::report().contains(QStringLiteral("SecurityException")) &&
                  !ErrorLog::report().contains(QStringLiteral("persistent private secret")), "restore safe history");
        } else if (mode == "--read-remote") {
            check(enabled && ErrorLog::detailedReport(&text, &error) &&
                  text.contains(QStringLiteral("remote private secret")), "restore remote detailed history");
            check(ErrorLog::report().contains(QStringLiteral("Android runtime error: SecurityException")) &&
                  !ErrorLog::report().contains(QStringLiteral("remote private secret")), "restore remote safe history");
        } else if (mode == "--clear") {
            check(enabled && ErrorLog::setDetailedEnabled(false, &error), "persist disabling detailed capture");
        } else if (mode == "--disabled") {
            check(!enabled && ErrorLog::setDetailedEnabled(true, &error), "restore disabled state");
            check(ErrorLog::detailedReport(&text, &error) &&
                  !text.contains(QStringLiteral("persistent private secret")), "cleared detailed history stays cleared");
        } else {
            check(false, "unknown test mode");
        }
        return 0;
    }
    if (argc > 1) {
        QCoreApplication app(argc, argv);
        QString error;
        check(ErrorLog::initialize(QStringLiteral("standalone"), &error), "initialize standalone");
        if (QByteArray(argv[1]) == "--standalone-file" || QByteArray(argv[1]) == "--standalone-file-error") {
            QString path;
            const bool started = ErrorLog::startStandaloneDiagnosticFile(&path, &error);
            if (QByteArray(argv[1]) == "--standalone-file-error") {
                check(!started && !error.isEmpty() && !ErrorLog::standaloneDiagnosticFileError().isEmpty(),
                      "standalone file errors are exposed");
                return 0;
            }
            check(started && !path.isEmpty(), "start standalone diagnostic file");
            bool locallyEnabled = true;
            check(ErrorLog::detailedEnabled(&locallyEnabled, &error) && !locallyEnabled,
                  "standalone file permission does not enable logging by itself");
            QDBusConnection bus = QDBusConnection::sessionBus();
            Identity identity;
            bool extended = true;
            check(bus.registerService(QStringLiteral("ru.omp.am")) &&
                  bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) &&
                  bus.registerService(QStringLiteral("ru.androidcompatibilitylayer.appmanager")) &&
                  MemoryLogBus::startServer([](const QString &, const QString &, const QString &,
                                               bool, quint64) {}, [&]() { return extended; }, &error) &&
                  ErrorLog::setDetailedEnabled(extended, &error) &&
                  ErrorLog::startForwarding(&error), "start controlled standalone log service");
            auto waitForState = [&](bool enabled) {
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 5000) {
                    app.processEvents();
                    if (MemoryLogBus::clientDetailedEnabled() == enabled) return true;
                    QThread::msleep(10);
                }
                return false;
            };
            check(waitForState(true), "wait for confirmed detailed state");
            DiagnosticLog diagnostic;
            check(diagnostic.start(&error, true), "capture standalone stderr");
            check(ErrorLog::startCoreChannel(&error), "start standalone Android log channel");
            ErrorLog::sendCoreLine(MEGLER_LOG_INFO, "store runtime test event");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("standalone file private test"), false);
            ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("GPU test event"), false);
            std::fputs("Log IPC manager identification: access denied\n", stderr);
            std::fflush(stderr);
            extended = false;
            check(ErrorLog::setDetailedEnabled(extended, &error), "disable manager-owned detailed state");
            check(waitForState(false), "stop standalone capture when manager disables it");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private event while disabled"), false);
            extended = true;
            check(ErrorLog::setDetailedEnabled(extended, &error), "enable manager-owned detailed state");
            check(waitForState(true), "resume standalone capture when manager enables it");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private event after re-enable"), false);
            extended = false;
            check(ErrorLog::setDetailedEnabled(extended, &error), "disable manager-owned state a second time");
            check(waitForState(false), "stop standalone capture a second time");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private event while disabled again"), false);
            extended = true;
            check(ErrorLog::setDetailedEnabled(extended, &error), "enable manager-owned state a second time");
            check(waitForState(true), "resume standalone capture a second time");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private event after second re-enable"), false);
            QTimer::singleShot(6500, &app, &QCoreApplication::quit);
            return app.exec();
        }
        check(ErrorLog::startForwarding(&error), "start standalone forwarding");
        if (QByteArray(argv[1]) == "--gpu-after-disable") {
            QTimer::singleShot(500, []() {
                MemoryLogBus::enqueue(QStringLiteral("Qt"), QString::fromLatin1(RemoteGpuSummary), false);
                MemoryLogBus::enqueue(QStringLiteral("Qt"), QStringLiteral("GPU Canvas fallback (details omitted)"), false);
            });
            QTimer::singleShot(1500, &app, &QCoreApplication::quit);
            return app.exec();
        }
        QDBusMessage read = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/log"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.log"), QStringLiteral("Read"));
        check(QDBusConnection::sessionBus().call(read).type() == QDBusMessage::ErrorMessage,
              "remote log reading is unavailable");
        QTimer::singleShot(500, []() {
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("SecurityException remote private secret"), true);
        });
        QTimer::singleShot(1500, &app, &QCoreApplication::quit);
        return app.exec();
    }
    QTemporaryDir home;
    check(home.isValid(), "temporary home");
    qputenv("HOME", home.path().toUtf8());
    QCoreApplication app(argc, argv);
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    check(QDir().mkpath(documents), "create documents");
    QTemporaryDir standaloneHome;
    check(standaloneHome.isValid(), "temporary standalone home");
    QProcessEnvironment standaloneEnvironment = QProcessEnvironment::systemEnvironment();
    standaloneEnvironment.insert(QStringLiteral("HOME"), standaloneHome.path());
    QProcess standaloneFile;
    standaloneFile.setProcessEnvironment(standaloneEnvironment);
    standaloneFile.setProcessChannelMode(QProcess::ForwardedChannels);
    standaloneFile.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-file-error"));
    check(standaloneFile.waitForFinished(5000) && standaloneFile.exitCode() == 0,
          "unavailable Documents is reported");
    check(QDir().mkpath(standaloneHome.path() + QStringLiteral("/Documents")), "create standalone Documents");
    standaloneFile.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-file"));
    check(standaloneFile.waitForStarted(5000), "start standalone file process");
    QElapsedTimer fileTimer;
    fileTimer.start();
    QByteArray standaloneContents;
    const QDir outputDirectory(standaloneHome.path() +
                               QStringLiteral("/Documents/AndroidCompatibilityLayer-Error-Logs"));
    while (fileTimer.elapsed() < 30000 && !standaloneContents.contains("private event after second re-enable")) {
        const bool finished = standaloneFile.state() == QProcess::NotRunning;
        const QStringList files = outputDirectory.entryList(QStringList() << QStringLiteral("*.txt"), QDir::Files);
        if (files.size() == 1) {
            QFile file(outputDirectory.filePath(files.first()));
            if (file.open(QIODevice::ReadOnly)) standaloneContents = file.readAll();
        }
        if (finished) break;
        QThread::msleep(25);
    }
    if (!standaloneContents.contains("private event after second re-enable") ||
        standaloneContents.contains("private event while disabled"))
        std::fprintf(stderr, "standalone diagnostic contents:\n%s\n", standaloneContents.constData());
    check(standaloneContents.contains("private event after second re-enable") &&
          !standaloneContents.contains("private event while disabled"),
          "periodic legacy diagnostic file follows confirmed off/on state");
    check(standaloneFile.waitForFinished(5000) && standaloneFile.exitCode() == 0 &&
          outputDirectory.entryList(QStringList() << QStringLiteral("*.txt"), QDir::Files).size() == 1,
          "standalone uses one file and flushes on exit");
    for (int i = 0; i < 7; ++i) {
        QFile oldFile(outputDirectory.filePath(QStringLiteral("StandaloneDiagnostic-existing-%1.txt").arg(i)));
        check(oldFile.open(QIODevice::WriteOnly) && oldFile.write("old") == 3, "create old diagnostic file");
    }
    standaloneFile.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-file-error"));
    check(standaloneFile.waitForFinished(5000) && standaloneFile.exitCode() == 0 &&
          outputDirectory.entryList(QStringList() << QStringLiteral("StandaloneDiagnostic-*.txt"), QDir::Files).size() == 8,
          "file limit is reported without deleting previous diagnostics");
    const QString oldDir = home.path() + QStringLiteral("/old");
    check(QDir().mkpath(oldDir), "create old log directory");
    const QStringList names = {QStringLiteral("activity.log"), QStringLiteral("detailed.log")};
    for (const QString &name : names) {
        QFile old(oldDir + QLatin1Char('/') + name);
        check(old.open(QIODevice::WriteOnly) && old.write("old log") == 7, "create old log");
    }
    QString error;
    check(ErrorLog::initialize(QStringLiteral("Android App Manager"), &error, oldDir), "initialize");
    const QString reportHeader = ErrorLog::report().section(QLatin1Char('\n'), 0, 5);
#if defined(Q_PROCESSOR_ARM_64)
    const QString expectedArchitecture = QStringLiteral("aarch64");
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
    const QString expectedArchitecture = QStringLiteral("armv7hl");
#elif defined(Q_PROCESSOR_X86_64)
    const QString expectedArchitecture = QStringLiteral("x86_64");
#else
    const QString expectedArchitecture = QStringLiteral("unknown");
#endif
    check(reportHeader.contains(QStringLiteral("Package architecture (build): ") + expectedArchitecture) &&
          !reportHeader.contains(QStringLiteral("\nArchitecture:")),
          "report identifies package build architecture rather than device architecture");
    const QString mainPrivateRoot = home.path() + QStringLiteral("/live-manager");
    check(ErrorLog::startPersistence(mainPrivateRoot, &error), "start manager storage");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private secret"), true);
    check(!ErrorLog::detailedCaptureEnabled(), "detailed capture starts disabled");
    check(!ErrorLog::report().contains(QStringLiteral("private secret")), "safe report redacts message");
    QString disabledPath;
    check(!ErrorLog::report().contains(QStringLiteral("Android runtime error")) &&
          !ErrorLog::saveReport(&disabledPath, &error) && disabledPath.isEmpty(),
          "logging off keeps no local events and cannot export");
    const QString gpuSummary = QStringLiteral("GPU Canvas performance: interval_ms=10000 frames=123 failed=0 samples=12 "
        "producer_us=123 producer_max_us=25 qt_wait_us=45 qt_wait_max_us=5 "
        "readbacks=2 readback_samples=2 readback_us=20 readback_max_us=15");
    const QString fullGpuSummary = QString::fromLatin1(FullGpuSummary);
    const QString canvasCopySummary = fullGpuSummary + QStringLiteral(" canvas_copies_gpu=3");
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary, false);
    check(!ErrorLog::report().contains(gpuSummary), "GPU summary omitted while detailed capture is disabled");
    check(ErrorLog::setDetailedEnabled(true, &error), "enable detailed capture");
    check(ErrorLog::detailedCaptureEnabled(), "detailed capture state is visible");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("droidmedia started: PRIVATE-DETAIL"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("uninteresting output"), false);
    ErrorLog::record(QStringLiteral("worker stderr"), QStringLiteral("Shared microG client ordered broad"), false);
    ErrorLog::record(QStringLiteral("worker stderr"), QStringLiteral("cast dispatch failed: PRIVATE-INTENT\n"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("droidmedia started")) &&
          ErrorLog::overlayText().contains(QStringLiteral("ordered broadcast")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("uninteresting")), "overlay captures filtered redacted complete lines");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG client account result: cancelled"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG client ordered broadcast received"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("droidmedia stop timed out; disabling hardware decoder for this process"), false);
    ErrorLog::record(QStringLiteral("controller stderr"), QStringLiteral("ACL codec: first hardware picture\nACL codec: video software decoder selected\n"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("account result: cancelled")) &&
          ErrorLog::overlayText().contains(QStringLiteral("ordered broadcast received")) &&
          ErrorLog::overlayText().contains(QStringLiteral("droidmedia stop timed out")) &&
          ErrorLog::overlayText().contains(QStringLiteral("first hardware picture")) &&
          ErrorLog::overlayText().contains(QStringLiteral("video software decoder selected")),
          "overlay distinguishes account, broadcast and decoder outcomes");
    const char *cameraStills[][2] = {
        {"camera: Rockchip front stills use viewfinder JPEG at QSize(2592, 1944)", "Camera: front viewfinder still enabled"},
        {"camera: viewfinder still JPEG captured", "Camera: front JPEG captured"},
        {"camera: viewfinder still could not be encoded", "Camera: front JPEG failed"},
        {"camera: Rockchip front viewfinder JPEG unavailable", "Camera: front JPEG unavailable"},
    };
    for (const auto &event : cameraStills) {
        ErrorLog::record(QStringLiteral("Qt"), QString::fromLatin1(event[0]) + QStringLiteral(" PRIVATE"), false);
        check(ErrorLog::overlayText().contains(QString::fromLatin1(event[1])) &&
              !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE")),
              "camera still overlay reports fixed events without raw details");
    }
    const char *codecFailures[][2] = {
        {"ACL codec: rockchip mpp decoder initialized", "Codec HW: rockchip mpp decoder initialized"},
        {"ACL codec: rockchip mpp first picture", "Codec HW: rockchip mpp first picture"},
        {"ACL codec: rockchip mpp capability rejected", "Codec HW: rockchip mpp capability rejected"},
        {"ACL codec: rockchip mpp buffer synchronization failed", "Codec HW: rockchip mpp buffer synchronization failed"},
        {"ACL codec: rockchip mpp output frame error", "Codec HW: rockchip mpp output frame error"},
        {"ACL codec: hardware replay limit reached", "Codec HW: replay limit reached"},
        {"ACL codec: droidmedia hardware fallback active", "Codec HW: droidmedia fallback active"},
        {"ACL codec: droidmedia decoder or FFmpeg 8 unavailable", "decoder or FFmpeg 8 unavailable"},
        {"ACL codec: droidmedia library or ABI unavailable", "library or ABI unavailable"},
        {"ACL codec: droidmedia capability check rejected", "capability check rejected"},
        {"ACL codec: droidmedia create or configure failed", "create or configure failed"},
        {"ACL codec: droidmedia start failed", "start failed"},
        {"ACL codec: droidmedia decoder initialization failed", "Codec HW: droidmedia initialization failed"},
        {"ACL codec: droidmedia queue timeout", "queue timeout"},
        {"ACL codec: droidmedia queue wait failed", "queue wait failed"},
        {"ACL codec: droidmedia queue failed", "queue failed"},
        {"ACL codec: droidmedia drain timeout", "drain timeout"},
        {"ACL codec: droidmedia drain wait failed", "drain wait failed"},
        {"ACL codec: droidmedia drain failed", "drain failed"},
        {"ACL codec: droidmedia dequeue failed", "dequeue failed"},
        {"Unsupported droidmedia output color format 0x1234", "unsupported output format"},
        {"ACL codec: droidmedia output frame rejected", "output frame rejected"},
        {"ACL codec: droidmedia asynchronous error", "asynchronous decoder error"},
        {"ACL codec: droidmedia replay limit reached", "replay limit reached"},
        {"ACL codec: AAC FFmpeg 62 initialization failed (-1)", "Codec AAC: FFmpeg 8 initialization failed"},
        {"ACL codec: AAC FFmpeg 61 initialization failed (-1)", "Codec AAC: FFmpeg 7 initialization failed"},
        {"ACL codec: video FFmpeg 62 initialization failed (-1)", "Codec video: FFmpeg 8 initialization failed"},
        {"ACL codec: video FFmpeg 61 initialization failed (-1)", "Codec video: FFmpeg 7 initialization failed"},
        {"AndroidCompatibilityLayer audio codec: decoder initialization failed", "Codec: decoder initialization failed"},
        {"droidmedia disabled after a stop failure", "disabled after stop failure"},
        {"cannot start bounded droidmedia stop", "stop worker unavailable"},
    };
    for (const auto &failure : codecFailures) {
        const QString line = QString::fromLatin1(failure[0]) + QStringLiteral(" PRIVATE-DATA\n");
        ErrorLog::record(QStringLiteral("controller stderr"), line.left(12), false);
        ErrorLog::record(QStringLiteral("controller stderr"), line.mid(12), false);
        check(ErrorLog::overlayText().contains(QString::fromLatin1(failure[1])) &&
              !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-DATA")),
              "standalone stderr overlay shows a fixed codec failure without raw data");
    }
    const QString beforeGoogleStartup = ErrorLog::overlayText();
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Google services already running"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG identity already started"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Google checkin completed without error"), false);
    check(ErrorLog::overlayText() == beforeGoogleStartup,
          "normal Google startup and unrelated text do not imply sign-in failure");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG service bind failed. PRIVATE-DETAIL"), true);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Google account is unavailable"), true);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Another Google sign-in is active or the account screen is unavailable"), true);
    check(ErrorLog::overlayText().contains(QStringLiteral("Google / microG: bind failed")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Google / microG: requested account unavailable")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Google / microG: sign-in busy or screen unavailable")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE")),
          "Google overlay reports fixed reasons without guessing or exposing details");
    const char *gmsStartup[][2] = {
        {"Shared microG client channel ready.", "client channel ready"},
        {"Shared microG client channel open failed.", "client channel open failed"},
        {"W/GmsClient: unable to connect to service: PRIVATE-ACTION", "GMS service connection failed"},
        {"W/ServiceBindIntentUtils: Dynamic intent resolution failed: PRIVATE-DETAIL", "dynamic service resolution failed"},
        {"E/DynamiteModule: Invalid GmsCore APK, remote loading disabled.", "GMS module package unavailable"},
    };
    for (const auto &event : gmsStartup) {
        ErrorLog::record(QStringLiteral("Android"), QString::fromLatin1(event[0]), false);
        check(ErrorLog::overlayText().contains(QStringLiteral("Google / microG: ") + QString::fromLatin1(event[1])) &&
              !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE")),
              "GMS startup overlay distinguishes channel, binding, resolution and module failures");
    }
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("Shared microG module APK ready."), false);
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("Shared microG module APK unavailable: PRIVATE-APK-DETAIL"), true);
    check(ErrorLog::overlayText().contains(QStringLiteral("microG module: APK ready")) &&
          ErrorLog::overlayText().contains(QStringLiteral("microG module: APK unavailable")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-APK-DETAIL")),
          "overlay reports module APK availability without details or TLS provider claims");
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("Experimental GPU Canvas active; Android 2D draws into a shared GLES texture"), false);
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("Experimental GPU Canvas falling back to CPU: shared OpenGL context is unavailable"), true);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: job started"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: job finished"), false);
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("ACL notification: posted"), false);
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("ACL notification: removed"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("GPU Canvas active")) &&
          ErrorLog::overlayText().contains(QStringLiteral("shared context unavailable")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Background job: started")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Background job: finished")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: posted")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: removed")),
          "overlay reports GPU, background work and notification transitions");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("System notification service is unavailable (D-Bus org.example.Error: PRIVATE-DATA)."), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("System notification permission denied (D-Bus org.freedesktop.DBus.Error.AccessDenied: PRIVATE-DATA)."), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("System notification post failed (PRIVATE-DATA)."), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("System notification update failed (PRIVATE-DATA)."), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("System notification cancellation failed."), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Android notification limit reached."), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("Android notification: D-Bus unavailable")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: permission denied")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: post failed")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: update failed")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: cancel failed")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Android notification: limit reached")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-DATA")),
          "notification overlay reports fixed failure reasons without raw details");
    const QString beforeBenignNotificationText = ErrorLog::overlayText();
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("notification classification is unavailable in this runtime"), false);
    check(ErrorLog::overlayText() == beforeBenignNotificationText,
          "notification overlay ignores normal fallback");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG disconnect notification failed"), false);
    check(ErrorLog::overlayText().endsWith(QStringLiteral("Crash or bridge error")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("Android notification: error")),
          "unrelated microG notification text is not classified as a notification failure");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: job stopped"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("Background job: cancelled by app")),
          "job stop reports the only modeled stop cause");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: foreground service stopped"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("Foreground service: finished")),
          "normal foreground-service completion is not shown as an error");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: WorkManager job started"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: WorkManager job finished"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("ACL background: WorkManager job stopped"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("WorkManager job: started")) &&
          ErrorLog::overlayText().endsWith(QStringLiteral("WorkManager job: finished")),
          "normal WorkManager stop is displayed as finished");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("media: extractor: 0 compressed tracks"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("media: extractor: 12 compressed tracks"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("Media extractor: no compressed tracks")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Media extractor: compressed tracks available")),
          "overlay distinguishes empty and populated compressed extractors");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG Binder descriptors sent PRIVATE-FD"), false);
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("Shared microG Binder descriptors received PRIVATE-FD"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("Shared microG Binder descriptors sent")) &&
          ErrorLog::overlayText().contains(QStringLiteral("Shared microG Binder descriptors received")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-FD")),
          "overlay distinguishes descriptor transfer directions without exposing details");
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("WebView GPU: native EGL texture capture ready PRIVATE-WEBVIEW"), false);
    ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("WebView GPU: native EGL texture capture unavailable; software fallback PRIVATE-WEBVIEW"), false);
    check(ErrorLog::overlayText().contains(QStringLiteral("WebView GPU: capture ready")) &&
          ErrorLog::overlayText().contains(QStringLiteral("WebView GPU: software fallback")) &&
          !ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-WEBVIEW")),
          "overlay distinguishes WebView GPU capability and software fallback without details");
    for (int i = 0; i < 10; ++i)
        ErrorLog::record(QStringLiteral("Android"), QStringLiteral("notification delivery failed"), false);
    check(ErrorLog::overlayText().split(QLatin1Char('\n')).size() == 6 &&
          !ErrorLog::overlayText().contains(QStringLiteral("droidmedia started")),
          "overlay retains only the most recent bounded events");
    ErrorLog::record(QStringLiteral("PauseTest"), QStringLiteral("before hold"), false);
    check(ErrorLog::setDetailedPaused(true) && !ErrorLog::detailedCaptureEnabled(), "pause detailed capture");
    check(ErrorLog::overlayText().isEmpty(), "overlay hidden when capture is paused");
    ErrorLog::record(QStringLiteral("PauseTest"), QStringLiteral("during hold"), false);
    QString pausedReport;
    check(ErrorLog::detailedReport(&pausedReport, &error) &&
          pausedReport.contains(QStringLiteral("before hold")) &&
          !pausedReport.contains(QStringLiteral("during hold")), "pause retains previous events without recording new ones");
    check(ErrorLog::setDetailedPaused(false) && ErrorLog::detailedCaptureEnabled(), "resume detailed capture");
    ErrorLog::record(QStringLiteral("PauseTest"), QStringLiteral("after hold"), false);
    check(ErrorLog::detailedReport(&pausedReport, &error) &&
          pausedReport.contains(QStringLiteral("before hold")) &&
          pausedReport.contains(QStringLiteral("after hold")) &&
          !pausedReport.contains(QStringLiteral("during hold")), "resume retains prior events");
    const QString rpmEvent = QStringLiteral("Standalone RPM: pid=123 request=1 event=stage-build");
    ErrorLog::record(QStringLiteral("Qt"), rpmEvent, false);
    ErrorLog::record(QStringLiteral("Qt"), rpmEvent + QStringLiteral(" PRIVATE-RPM"), false);
    ErrorLog::record(QStringLiteral("Qt"),
                     QStringLiteral("Standalone RPM: pid=123 request=1 event=stage-PRIVATE-RPM"), false);
    check(ErrorLog::report().contains(rpmEvent) &&
          !ErrorLog::report().contains(QStringLiteral("PRIVATE-RPM")),
          "safe RPM events are strictly filtered");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("another private secret"), true);
    check(ErrorLog::report().contains(QStringLiteral("Android runtime error")) &&
          !ErrorLog::report().contains(QStringLiteral("another private secret")), "safe report retains only error category");
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary + QStringLiteral(" PRIVATE-GPU"), false);
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary, false);
    {
        DiagnosticLog localDiagnostics;
        check(localDiagnostics.start(&error, false), "start local Qt diagnostics");
        qInfo("%s", qPrintable(canvasCopySummary));
    }
    ErrorLog::record(QStringLiteral("Qt"), fullGpuSummary + QStringLiteral(" PRIVATE-GPU"), false);
    const QString partialCacheSummary = fullGpuSummary.left(fullGpuSummary.lastIndexOf(QLatin1Char(' ')));
    ErrorLog::record(QStringLiteral("Qt"), partialCacheSummary, false);
    const QString extendedGpuSummary = gpuSummary + QStringLiteral(
        " readback_flush=1 readback_filter=2 readback_self=3 readback_gl_us=4 readback_convert_us=5");
    const QString presentGpuSummary = extendedGpuSummary + QStringLiteral(" readback_present=6");
    ErrorLog::record(QStringLiteral("Qt"), extendedGpuSummary, false);
    ErrorLog::record(QStringLiteral("Qt"), presentGpuSummary, false);
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary + QStringLiteral(" readback_flush=1 PARTIAL-GPU"), false);
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary + QStringLiteral(" readback_present=6"), false);
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary + QStringLiteral(" readback_present=6 PARTIAL-PRESENT"), false);
    check(ErrorLog::report().contains(gpuSummary) &&
          ErrorLog::report().contains(extendedGpuSummary) &&
          ErrorLog::report().contains(presentGpuSummary) &&
          ErrorLog::report().contains(canvasCopySummary) &&
          !ErrorLog::report().contains(partialCacheSummary + QLatin1Char('\n')) &&
          !ErrorLog::report().contains(QStringLiteral("PRIVATE-GPU")) &&
          !ErrorLog::report().contains(QStringLiteral("PARTIAL-GPU")) &&
          !ErrorLog::report().contains(gpuSummary + QStringLiteral(" readback_present=6")) &&
          !ErrorLog::report().contains(QStringLiteral("PARTIAL-PRESENT")), "safe GPU summary is strictly filtered");
    QString detailed;
    QDBusConnection bus = QDBusConnection::sessionBus();
    Identity identity;
    QTemporaryDir sharedLogRoot;
    check(sharedLogRoot.isValid(), "create organization snapshot root");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("Organization snapshots are not configured.")),
          "uninitialized snapshot root is reported");
    check(!ErrorLog::setStandaloneSnapshotRoot(sharedLogRoot.path() + QStringLiteral("/missing"), &error) &&
          !error.isEmpty() && ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("Organization files directory is unavailable.")),
          "snapshot root failure is reported without exposing its path");
    check(bus.registerService(QStringLiteral("ru.omp.am")) &&
          bus.registerVirtualObject(QStringLiteral("/ru/omp/am"), &identity) &&
          bus.registerService(QStringLiteral("ru.androidcompatibilitylayer.appmanager")) &&
          ErrorLog::startCollector(&error), "start manager collector");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("No requests: standalone may not have started")),
          "missing standalone connection is visible in detailed report");
    QProcess child;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&child, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("child"));
    timeout.start(5000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0, "standalone process completed");
    check(ErrorLog::report().contains(QStringLiteral("Android runtime error: SecurityException")) &&
          !ErrorLog::report().contains(QStringLiteral("remote private secret")), "remote safe report redacted");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-snapshot")
                << sharedLogRoot.path());
    timeout.start(5000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0,
          "snapshot-only standalone process completed");
    const QString snapshotDirectory = sharedLogRoot.path() +
        QStringLiteral("/AndroidCompatibilityLayer-Standalone-Logs");
    const QStringList snapshotNames = QDir(snapshotDirectory).entryList(
        QStringList() << QStringLiteral("standalone-*.log"), QDir::Files);
    check(snapshotNames.size() == 1, "standalone publishes one per-instance snapshot");
    QString standaloneSnapshotPath = QDir(snapshotDirectory).filePath(snapshotNames.first());
    QFile snapshotFile(standaloneSnapshotPath);
    check(snapshotFile.open(QIODevice::ReadOnly) &&
          snapshotFile.readAll().count("snapshot-only private event") == 1,
          "snapshot contains the standalone event once");
    check(ErrorLog::setStandaloneSnapshotRoot(sharedLogRoot.path(), &error),
          "configure snapshot root when background manager opens");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.count(QStringLiteral("snapshot-only private event")) == 1 &&
          detailed.contains(QStringLiteral("untrusted shared file")) &&
          detailed.contains(QStringLiteral("organization apps may access it")),
          "manager view reads standalone records from the shared file exactly once");
    {
        ErrorLog::record(QStringLiteral("test"), QStringLiteral("feed newer manager line"), false);
        QString feed;
        bool more = false;
        check(ErrorLog::recentLines(QString(), 200, &feed, &more, &error) &&
              feed.contains(QStringLiteral("[Android0123456789abcdef01234567] [Android] snapshot-only private event")) &&
              feed.indexOf(QStringLiteral("feed newer manager line")) <
                  feed.indexOf(QStringLiteral("snapshot-only private event")),
              "log feed merges standalone lines with manager lines, newest first");
        check(ErrorLog::recentLines(QStringLiteral("SNAPSHOT-ONLY"), 200, &feed, &more, &error) &&
              feed.count(QLatin1Char('\n')) == 1 && !more,
              "log feed search returns only matching standalone lines");
        check(ErrorLog::recentLines(QString(), 1, &feed, &more, &error) &&
              feed.count(QLatin1Char('\n')) == 1 && more,
              "log feed respects its line limit");
    }
    check(!ErrorLog::report().contains(QStringLiteral("snapshot-only private event")),
          "snapshot-only standalone content does not enter the D-Bus event history");
    ErrorLog::record(QStringLiteral("test"), QStringLiteral("detailed secret"), false);
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("detailed secret")) &&
          detailed.contains(fullGpuSummary) &&
          detailed.contains(fullGpuSummary + QStringLiteral(" PRIVATE-GPU")),
          "full local GPU metrics are retained only in the opt-in detailed report");
    check(!ErrorLog::report().contains(QStringLiteral("PRIVATE-GPU")),
          "unstructured local GPU payload is excluded from the safe report");
    check(detailed.contains(QStringLiteral("remote private secret")), "remote detailed event collected");
    check(detailed.contains(QStringLiteral("Log transport (manager process only):")) &&
          detailed.contains(QStringLiteral("Client diagnostics (untrusted, no log data):")) &&
          detailed.contains(QStringLiteral("probe / started = ")) &&
          !detailed.contains(QStringLiteral("append requests=0")) &&
          !detailed.contains(QStringLiteral("test-key")) &&
          !detailed.contains(QStringLiteral("/usr/bin/ru.androidcompatibilitylayer.Android")),
          "transport status is present without remote identity");
    const QString liveSnapshot = mainPrivateRoot + QStringLiteral("/diagnostic-log/manager-state-v1.dat");
    QElapsedTimer persistenceTimer;
    persistenceTimer.start();
    bool remotePersisted = false;
    while (persistenceTimer.elapsed() < 10000 && !remotePersisted) {
        app.processEvents();
        QFile snapshot(liveSnapshot);
        if (snapshot.open(QIODevice::ReadOnly))
            remotePersisted = snapshot.readAll().contains("remote private secret");
        if (!remotePersisted) QThread::msleep(25);
    }
    check(remotePersisted, "remote events saved automatically");
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--read-remote") << mainPrivateRoot) == 0,
          "restore remote events in new process");
    check(!QFileInfo::exists(documents + QStringLiteral("/AndroidCompatibilityLayer-Error-Logs")),
          "no export directory before explicit save");
    for (const QString &name : names) {
        QFile old(oldDir + QLatin1Char('/') + name);
        check(old.open(QIODevice::ReadOnly) && old.readAll() == QByteArray("old log"), "old log unchanged");
    }
    const QString failedExportTarget = QDir(documents).filePath(QStringLiteral("AndroidCompatibilityLayer-Error-Logs"));
    QFile failedExportBlocker(failedExportTarget);
    check(failedExportBlocker.open(QIODevice::WriteOnly) &&
          failedExportBlocker.write("block") == 5, "create export failure fixture");
    QString failedPath, cleanupWarning;
    bool partialExport = false;
    check(!ErrorLog::saveDetailedReport(&failedPath, &error, &partialExport, &cleanupWarning) &&
          failedPath.isEmpty() && QFileInfo::exists(standaloneSnapshotPath),
          "failed atomic export preserves shared standalone source");
    failedExportBlocker.close();
    check(QFile::remove(failedExportTarget), "remove export failure fixture");
    QString path;
    check(ErrorLog::saveDetailedReport(&path, &error, &partialExport, &cleanupWarning) &&
          !partialExport && cleanupWarning.isEmpty(), "explicit detailed export");
    check(QFileInfo(path).absolutePath() ==
              QDir(documents).filePath(QStringLiteral("AndroidCompatibilityLayer-Error-Logs")),
          "detailed export is saved in Documents");
    QFile exported(path);
    check(exported.open(QIODevice::ReadOnly), "open detailed export");
    const QByteArray exportedContents = exported.readAll();
    check(exportedContents.contains("detailed secret") &&
              exportedContents.contains("Client diagnostics (untrusted, no log data):") &&
              exportedContents.contains("[Remote source 1; events are untrusted]") &&
              exportedContents.contains("remote private secret") &&
              exportedContents.count("snapshot-only private event") == 1,
          "single detailed export contains manager, D-Bus and shared standalone sections once");
    check(!QFileInfo::exists(standaloneSnapshotPath),
          "successfully exported standalone snapshot is removed from organization storage");
    const QString snapshotLink = QDir(snapshotDirectory).filePath(
        QStringLiteral("standalone-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.log"));
    QFile snapshotLinkTarget(home.path() + QStringLiteral("/snapshot-link-target"));
    check(snapshotLinkTarget.open(QIODevice::WriteOnly) && snapshotLinkTarget.write("not a log") == 9 &&
          snapshotLinkTarget.link(snapshotLink), "create untrusted snapshot symlink fixture");
    QString partialPath;
    check(ErrorLog::saveDetailedReport(&partialPath, &error, &partialExport, &cleanupWarning) &&
          partialExport && QFileInfo(snapshotLink).isSymLink(),
          "partial export reports and preserves a symlink snapshot");
    QFile partialExportFile(partialPath);
    check(partialExportFile.open(QIODevice::ReadOnly) &&
          partialExportFile.readAll().contains("Standalone export partial"),
          "partial export marker is written to the file");
    check(QFile::remove(partialPath) && QFile::remove(snapshotLink) && QFile::remove(snapshotLinkTarget.fileName()),
          "remove partial export fixtures");
    check(ErrorLog::saveDetailedReport(&path, &error, &partialExport, &cleanupWarning),
          "subsequent export succeeds after cleanup");
    QFile secondExport(path);
    check(secondExport.open(QIODevice::ReadOnly) &&
          secondExport.readAll().count("snapshot-only private event") == 0,
          "exported standalone lines do not reappear in the next file");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("No standalone snapshots found")),
          "empty configured snapshot directory is reported");
    const QString offlineDirectory = snapshotDirectory + QStringLiteral("-offline");
    check(QDir().rename(snapshotDirectory, offlineDirectory) &&
          ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("Organization snapshots unavailable:")) &&
          !detailed.contains(sharedLogRoot.path()) &&
          QDir().rename(offlineDirectory, snapshotDirectory),
          "unavailable snapshot directory is reported without exposing its path");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-snapshot-stress")
                << sharedLogRoot.path());
    timeout.start(60000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0,
          "large standalone snapshot writer completed");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("startup retained")) &&
          detailed.contains(QStringLiteral("important retained error")) &&
          detailed.contains(QStringLiteral("verbose event 6999")) &&
          !detailed.contains(QStringLiteral("Standalone snapshot entries dropped due to limits:")),
          "standalone snapshot retains 7000 events without loss");
    QString stressPath;
    check(ErrorLog::saveDetailedReport(&stressPath, &error, &partialExport, &cleanupWarning) &&
          !partialExport && QFile::remove(stressPath),
          "large standalone snapshot exports without loss");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-snapshot-overflow")
                << sharedLogRoot.path());
    timeout.start(60000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0,
          "overflowing standalone snapshot writer completed");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("startup retained")) &&
          detailed.contains(QStringLiteral("important retained error")) &&
          detailed.contains(QStringLiteral("Standalone snapshot entries dropped due to limits:")),
          "bounded snapshots retain startup, errors and recent events while reporting loss");
    check(ErrorLog::saveDetailedReport(&stressPath, &error, &partialExport, &cleanupWarning) &&
          partialExport, "truncated standalone snapshot exports as partial");
    QFile firstLargeExport(stressPath);
    check(firstLargeExport.open(QIODevice::ReadOnly), "open bounded export");
    const QByteArray firstLargeContents = firstLargeExport.readAll();
    check(firstLargeContents.size() <= 10000000 &&
          firstLargeContents.contains("startup retained") &&
          firstLargeContents.contains("verbose event 6999") &&
          QFile::remove(stressPath), "compacted append log stays within the report limit");
    check(ErrorLog::saveDetailedReport(&stressPath, &error, &partialExport, &cleanupWarning),
          "export after bounded standalone export");
    QFile secondLargeExport(stressPath);
    check(secondLargeExport.open(QIODevice::ReadOnly), "open second bounded export");
    check(!secondLargeExport.readAll().contains("verbose event 6999") && QFile::remove(stressPath),
          "exported standalone events are not repeated");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-snapshot-repeats")
                << sharedLogRoot.path());
    timeout.start(10000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0,
          "repeated error snapshot writer completed");
    check(ErrorLog::detailedReport(&detailed, &error) &&
          detailed.contains(QStringLiteral("[Repeated 999 additional times; last ")) &&
          detailed.contains(QStringLiteral("[Repeated 1000 additional times; last ")) &&
          detailed.count(QStringLiteral("identical error")) == 4 &&
          detailed.contains(QStringLiteral("[Android] error identical error")) &&
          detailed.contains(QStringLiteral("different error")) &&
          !detailed.contains(QStringLiteral("Standalone snapshot entries dropped due to limits:")),
          "consecutive identical errors retain counts across flushes and distinct errors");
    check(ErrorLog::saveDetailedReport(&stressPath, &error, &partialExport, &cleanupWarning) &&
          !partialExport && QFile::remove(stressPath), "export repeated error snapshot");
    const QString largeManagerEntry(60000, QLatin1Char('m'));
    for (int i = 0; i < 140; ++i)
        ErrorLog::record(QStringLiteral("test"), largeManagerEntry, false);
    const QString triggerPath = sharedLogRoot.path() + QStringLiteral("/update-trigger");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--standalone-snapshot-update")
                << sharedLogRoot.path() << triggerPath);
    check(child.waitForStarted(5000), "start concurrent snapshot writer");
    QElapsedTimer publishWait;
    publishWait.start();
    bool beforeSnapshotPublished = false;
    QString updateSnapshotPath;
    while (publishWait.elapsed() < 5000 && !beforeSnapshotPublished) {
        app.processEvents();
        const QStringList currentSnapshots = QDir(snapshotDirectory).entryList(
            QStringList() << QStringLiteral("standalone-*.log"), QDir::Files);
        if (currentSnapshots.size() == 1) {
            updateSnapshotPath = QDir(snapshotDirectory).filePath(currentSnapshots.first());
            QFile current(updateSnapshotPath);
            if (current.open(QIODevice::ReadOnly))
                beforeSnapshotPublished = current.readAll().contains("snapshot-before-export");
        }
        if (!beforeSnapshotPublished) QThread::msleep(5);
    }
    check(beforeSnapshotPublished, "publish snapshot before concurrent export");
    const QString lockPath = updateSnapshotPath.left(updateSnapshotPath.size() - 4) +
                             QStringLiteral(".lock");
    const QString raceTrigger = triggerPath;
    std::atomic<bool> triggerWritten(false);
    std::thread updateDuringExport([&]() {
        const int lockFd = ::open(QFile::encodeName(lockPath).constData(), O_RDWR | O_CLOEXEC);
        if (lockFd < 0) return;
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 5000 && !triggerWritten.load()) {
            if (::flock(lockFd, LOCK_EX | LOCK_NB) == 0) {
                ::flock(lockFd, LOCK_UN);
                QThread::msleep(1);
                continue;
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) break;
            const int triggerFd = ::open(QFile::encodeName(raceTrigger).constData(),
                                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (triggerFd >= 0) {
                ::write(triggerFd, "go", 2);
                ::close(triggerFd);
                triggerWritten.store(true);
            }
            break;
        }
        ::close(lockFd);
    });
    QString racePath;
    check(ErrorLog::saveDetailedReport(&racePath, &error, &partialExport, &cleanupWarning),
          "export snapshot while another process updates it");
    updateDuringExport.join();
    check(triggerWritten.load(), "writer update overlapped the export lock");
    QFile raceExport(racePath);
    check(raceExport.open(QIODevice::ReadOnly), "open concurrent export");
    const QByteArray raceContents = raceExport.readAll();
    check(raceContents.count("snapshot-before-export") == 1 &&
          raceContents.count("snapshot-during-export") == 0 &&
          raceContents.size() <= 10000000 &&
          !raceContents.contains("[Manager log truncated") &&
          !raceContents.contains("[Report truncated at 10000000 bytes"),
          "sub-limit combined export retains manager and locked snapshot data");
    check(child.waitForFinished(6000) && child.exitCode() == 0,
          "concurrent snapshot writer completes");
    QFile updatedSnapshot(updateSnapshotPath);
    check(updatedSnapshot.open(QIODevice::ReadOnly) &&
          updatedSnapshot.readAll().contains("snapshot-during-export"),
          "new snapshot survives export cleanup");
    QString updatedExportPath;
    check(ErrorLog::saveDetailedReport(&updatedExportPath, &error, &partialExport, &cleanupWarning),
          "export snapshot update after first save");
    QFile updatedExport(updatedExportPath);
    check(updatedExport.open(QIODevice::ReadOnly), "open updated export");
    const QByteArray updatedContents = updatedExport.readAll();
    check(updatedContents.count("snapshot-before-export") == 0 &&
          updatedContents.count("snapshot-during-export") == 1,
          "concurrent update is exported exactly once without repeating prior entries");
    check((exported.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                      QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0,
          "export is owner-only");
    check(ErrorLog::saveReport(&path, &error), "explicit short export while detailed logging is enabled");
    QFile shortExport(path);
    check(shortExport.open(QIODevice::ReadOnly), "open short export");
    const QByteArray shortContents = shortExport.readAll();
    check(shortContents.contains("Current process context:") &&
              !shortContents.contains("detailed secret") &&
              !shortContents.contains("remote private secret"),
          "short export omits raw local and remote messages");
    const QDir exportDir(QFileInfo(path).absolutePath());
    QFile standalone(exportDir.filePath(QStringLiteral("StandaloneDiagnostic-test.txt")));
    QFile unrelated(exportDir.filePath(QStringLiteral("notes.txt")));
    const QString link = exportDir.filePath(QStringLiteral("StandaloneDiagnostic-link.txt"));
    check(standalone.open(QIODevice::WriteOnly) && unrelated.open(QIODevice::WriteOnly) &&
          unrelated.link(link), "create saved-log cleanup fixtures");
    standalone.close();
    unrelated.close();
    const QString clearSnapshotPath = QDir(snapshotDirectory).filePath(
        QStringLiteral("standalone-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.log"));
    QFile clearSnapshot(clearSnapshotPath);
    check(clearSnapshot.open(QIODevice::WriteOnly) &&
          clearSnapshot.write("ACL-STANDALONE-LOG\t2\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t"
                              "ru.androidcompatibilitylayer.Android0123456789abcdef01234567\n") > 0,
          "create organization snapshot cleanup fixture");
    int removed = 0;
    check(ErrorLog::clearSavedLogs(removed) && removed == 7 &&
          !QFileInfo::exists(path) && !exported.exists() && !standalone.exists() &&
          !clearSnapshot.exists() &&
          unrelated.exists() && QFileInfo(link).isSymLink(), "clear only saved regular log files");
    check(ErrorLog::clearSavedLogs(removed) && removed == 0 &&
          ErrorLog::detailedReport(&detailed, &error) && detailed.contains(largeManagerEntry),
          "cleanup is repeatable and preserves the current journal");
    check(ErrorLog::clearCurrentLog(&error) && ErrorLog::detailedReport(&detailed, &error) &&
          !detailed.contains(largeManagerEntry), "clear current journal while capture remains enabled");
    exported.close();
    check(exported.open(QIODevice::WriteOnly) && exported.write("user export") > 0,
          "recreate exported log");
    exported.close();
    clearSnapshot.close();
    check(clearSnapshot.open(QIODevice::WriteOnly) && clearSnapshot.write("{}") > 0,
          "recreate shared snapshot");
    clearSnapshot.close();
    check(ErrorLog::clearSavedLogs(removed, false) && removed == 1 &&
          !clearSnapshot.exists() && exported.exists(), "clear shared snapshots without deleting exported files");
    check(ErrorLog::setDetailedEnabled(false, &error), "disable detailed capture");
    check(!ErrorLog::detailedCaptureEnabled(), "disabled detailed capture state is visible");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--gpu-after-disable"));
    timeout.start(5000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0 &&
          !ErrorLog::report().contains(QStringLiteral("GPU Canvas fallback (details omitted)")) &&
          !ErrorLog::report().contains(QString::fromLatin1(RemoteGpuSummary)),
          "manager ignores remote events when logging is disabled");
    check(!ErrorLog::detailedReport(&detailed, &error), "disabled detailed report unavailable");
    check(ErrorLog::setDetailedEnabled(true, &error), "re-enable detailed capture");
    check(ErrorLog::detailedReport(&detailed, &error) && !detailed.contains(QStringLiteral("detailed secret")),
          "disabled capture clears detailed memory");
    check(ErrorLog::startCoreChannel(&error), "start core channel");
    QByteArray longLine("core-long-start:");
    longLine += QByteArray(4059, 'x');
    longLine += "\xe2\x82\xac";
    longLine += QByteArray(2000, 'y');
    longLine += ":core-long-end";
    const QByteArray first = QByteArray("core-thread-a:") + QByteArray(6000, 'a') + ":end-a";
    const QByteArray second = QByteArray("core-thread-b:") + QByteArray(6000, 'b') + ":end-b";
    ErrorLog::sendCoreLine(MEGLER_LOG_ERROR, longLine.constData());
    std::thread firstSender([&]() { ErrorLog::sendCoreLine(MEGLER_LOG_INFO, first.constData()); });
    std::thread secondSender([&]() { ErrorLog::sendCoreLine(MEGLER_LOG_INFO, second.constData()); });
    firstSender.join();
    secondSender.join();
    const auto previousHandler = std::signal(SIGUSR1, coreLogSignalHandler);
    check(previousHandler != SIG_ERR && std::raise(SIGUSR1) == 0, "send core log from signal handler");
    check(std::signal(SIGUSR1, previousHandler) != SIG_ERR, "restore signal handler");
    for (int i = 0; i < 100; ++i) {
        const QByteArray event = QByteArray("core-burst-") + QByteArray::number(i) + ":end";
        ErrorLog::sendCoreLine(MEGLER_LOG_INFO, event.constData());
    }
    QElapsedTimer timer;
    timer.start();
    bool received = false;
    while (timer.elapsed() < 5000 && !received) {
        app.processEvents();
        received = ErrorLog::detailedReport(&detailed, &error) &&
            detailed.contains(QString::fromUtf8(longLine)) &&
            detailed.contains(QString::fromUtf8(first)) &&
            detailed.contains(QString::fromUtf8(second)) &&
            detailed.contains(QStringLiteral("core signal handler event")) &&
            detailed.contains(QStringLiteral("core-burst-99:end"));
        QThread::msleep(10);
    }
    check(received, "long UTF-8 and concurrent core messages are reassembled intact");
    for (int i = 0; i < 100; ++i)
        check(detailed.contains(QStringLiteral("core-burst-%1:end").arg(i)),
              "burst is buffered while the Qt event loop is not processing");
    const QByteArray oversized(10000001, 'z');
    ErrorLog::sendCoreLine(MEGLER_LOG_ERROR, oversized.constData());
    timer.restart();
    while (timer.elapsed() < 1500 && !ErrorLog::report().contains(QStringLiteral("events dropped"))) {
        app.processEvents();
        QThread::msleep(10);
    }
    check(ErrorLog::report().contains(QStringLiteral("events dropped")), "oversized core event reported");
    const QString quitRoot = home.path() + QStringLiteral("/core-quit");
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--core-quit") << quitRoot) == 0 &&
          QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--read-core-quit") << quitRoot) == 0,
          "persist queued core messages on normal shutdown");
    const QString privateRoot = home.path() + QStringLiteral("/manager-private");
    const QString snapshotPath = privateRoot + QStringLiteral("/diagnostic-log/manager-state-v1.dat");
    QProcess writer;
    writer.start(app.applicationFilePath(), QStringList() << QStringLiteral("--write") << privateRoot);
    check(writer.waitForStarted(5000), "start persistent manager process");
    timer.restart();
    bool persisted = false;
    while (timer.elapsed() < 10000 && !persisted) {
        QFile snapshot(snapshotPath);
        if (snapshot.open(QIODevice::ReadOnly))
            persisted = snapshot.readAll().contains("persistent private secret");
        if (!persisted) QThread::msleep(50);
    }
    check(persisted, "automatic snapshot persisted");
    check(QFileInfo(snapshotPath).size() <= 96 * 1024 * 1024, "snapshot size is bounded");
    writer.kill();
    check(writer.waitForFinished(5000), "terminate persistent manager abruptly");
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--read") << privateRoot) == 0,
          "restore after abrupt termination");
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--clear") << privateRoot) == 0 &&
          QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--disabled") << privateRoot) == 0,
          "restore cleared detailed history");
    QFile snapshot(snapshotPath);
    check((snapshot.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                     QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0,
          "snapshot is owner-only");
    const QString corruptRoot = home.path() + QStringLiteral("/corrupt-private");
    check(QDir().mkpath(corruptRoot + QStringLiteral("/diagnostic-log")), "create corrupt test directory");
    check(QFile::setPermissions(corruptRoot + QStringLiteral("/diagnostic-log"),
                                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "protect corrupt test directory");
    QFile corrupt(corruptRoot + QStringLiteral("/diagnostic-log/manager-state-v1.dat"));
    check(corrupt.open(QIODevice::WriteOnly) && corrupt.write("corrupt") == 7 &&
          corrupt.setPermissions(QFile::ReadOwner | QFile::WriteOwner), "create corrupt snapshot");
    corrupt.close();
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--corrupt") << corruptRoot) == 0,
          "corrupt snapshot is rejected");
    check(corrupt.open(QIODevice::ReadOnly) && corrupt.readAll() == QByteArray("corrupt"),
          "corrupt snapshot preserved");
    const QString invalidRoot = home.path() + QStringLiteral("/not-a-directory");
    QFile invalid(invalidRoot);
    check(invalid.open(QIODevice::WriteOnly), "create invalid private root");
    invalid.close();
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--bad-root") << invalidRoot) == 0,
          "storage setup errors are reported");
    check(QProcess::execute(app.applicationFilePath(), QStringList() << QStringLiteral("--fail-write")
                            << (home.path() + QStringLiteral("/write-failure"))) == 0,
          "storage write errors are reported");
    std::puts("Error log OK");
    return 0;
}
