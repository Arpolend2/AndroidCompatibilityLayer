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

#include <cstdio>

static void check(bool value, const char *description)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static const char RemoteGpuSummary[] =
    "GPU Canvas performance: interval_ms=42 frames=1 failed=0 samples=1 producer_us=1 "
    "producer_max_us=1 qt_wait_us=1 qt_wait_max_us=1 readbacks=0 readback_samples=0 "
    "readback_us=0 readback_max_us=0";

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
            check(text.contains(QStringLiteral("[Earlier detailed output was truncated.]")),
                  "bounded detailed history reports truncation");
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
            DiagnosticLog diagnostic;
            check(diagnostic.start(&error, true), "capture standalone stderr");
            check(ErrorLog::startCoreChannel(&error), "start standalone Android log channel");
            ErrorLog::sendCoreLine(MEGLER_LOG_INFO, "store runtime test event");
            ErrorLog::record(QStringLiteral("Android"), QStringLiteral("standalone file private test"), false);
            ErrorLog::record(QStringLiteral("Qt"), QStringLiteral("GPU test event"), false);
            std::fputs("Log IPC manager identification: access denied\n", stderr);
            std::fflush(stderr);
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
    while (fileTimer.elapsed() < 6200 && !standaloneContents.contains("standalone file private test")) {
        const QStringList files = outputDirectory.entryList(QStringList() << QStringLiteral("*.txt"), QDir::Files);
        if (files.size() == 1) {
            QFile file(outputDirectory.filePath(files.first()));
            if (file.open(QIODevice::ReadOnly)) standaloneContents = file.readAll();
        }
        QThread::msleep(25);
    }
    check(standaloneContents.contains("standalone file private test") &&
          standaloneContents.contains("store runtime test event") &&
          standaloneContents.contains("GPU test event") &&
          standaloneContents.contains("Log IPC manager identification: access denied"),
          "periodic file includes runtime and transport diagnostics before exit");
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
    const QString mainPrivateRoot = home.path() + QStringLiteral("/live-manager");
    check(ErrorLog::startPersistence(mainPrivateRoot, &error), "start manager storage");
    ErrorLog::record(QStringLiteral("Android"), QStringLiteral("private secret"), true);
    check(!ErrorLog::detailedCaptureEnabled(), "detailed capture starts disabled");
    check(!ErrorLog::report().contains(QStringLiteral("private secret")), "safe report redacts message");
    check(ErrorLog::report().contains(QStringLiteral("Android runtime error")), "safe report retains error");
    const QString gpuSummary = QStringLiteral("GPU Canvas performance: interval_ms=10000 frames=123 failed=0 samples=12 "
        "producer_us=123 producer_max_us=25 qt_wait_us=45 qt_wait_max_us=5 "
        "readbacks=2 readback_samples=2 readback_us=20 readback_max_us=15");
    const QString fullGpuSummary = QStringLiteral(
        "GPU Canvas performance: interval_ms=3000 frames=240 failed=2 samples=15 "
        "producer_us=123456 producer_max_us=24000 qt_wait_us=9876 qt_wait_max_us=4500 "
        "readbacks=12 readback_samples=1 readback_us=789 readback_max_us=789 "
        "readback_flush=3 readback_filter=4 readback_self=2 readback_gl_us=600 "
        "readback_convert_us=189 readback_present=3 "
        "pixel_submit_samples=1 pixel_submit_us=3000 pixel_submit_max_us=3000 "
        "filter_fallback_mode=1 filter_fallback_blend=2 filter_fallback_self=3 "
        "filter_fallback_gles=4 filter_fallback_invalid=5 filter_draw_errors=6 "
        "readback_cache_flush=0 readback_cache_filter=7 readback_cache_self=1 readback_cache_present=2");
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary, false);
    check(!ErrorLog::report().contains(gpuSummary), "GPU summary omitted while detailed capture is disabled");
    check(ErrorLog::setDetailedEnabled(true, &error), "enable detailed capture");
    check(ErrorLog::detailedCaptureEnabled(), "detailed capture state is visible");
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary + QStringLiteral(" PRIVATE-GPU"), false);
    ErrorLog::record(QStringLiteral("Qt"), gpuSummary, false);
    {
        DiagnosticLog localDiagnostics;
        check(localDiagnostics.start(&error, false), "start local Qt diagnostics");
        qInfo("%s", qPrintable(fullGpuSummary));
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
          ErrorLog::report().contains(fullGpuSummary) &&
          !ErrorLog::report().contains(partialCacheSummary + QLatin1Char('\n')) &&
          !ErrorLog::report().contains(QStringLiteral("PRIVATE-GPU")) &&
          !ErrorLog::report().contains(QStringLiteral("PARTIAL-GPU")) &&
          !ErrorLog::report().contains(gpuSummary + QStringLiteral(" readback_present=6")) &&
          !ErrorLog::report().contains(QStringLiteral("PARTIAL-PRESENT")), "safe GPU summary is strictly filtered");
    QString detailed;
    QDBusConnection bus = QDBusConnection::sessionBus();
    Identity identity;
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
    QString path;
    check(ErrorLog::saveDetailedReport(&path, &error), "explicit detailed export");
    check(QFileInfo(path).absolutePath() ==
              QDir(documents).filePath(QStringLiteral("AndroidCompatibilityLayer-Error-Logs")),
          "detailed export is saved in Documents");
    QFile exported(path);
    check(exported.open(QIODevice::ReadOnly), "open detailed export");
    const QByteArray exportedContents = exported.readAll();
    check(exportedContents.contains("detailed secret") &&
              exportedContents.contains("Client diagnostics (untrusted, no log data):") &&
              exportedContents.contains("[Remote source 1; events are untrusted]") &&
              exportedContents.contains("remote private secret"),
          "single detailed export contains manager and accepted standalone events");
    check((exported.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                      QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) == 0,
          "export is owner-only");
    check(ErrorLog::setDetailedEnabled(false, &error), "disable detailed capture");
    check(!ErrorLog::detailedCaptureEnabled(), "disabled detailed capture state is visible");
    child.start(app.applicationFilePath(), QStringList() << QStringLiteral("--gpu-after-disable"));
    timeout.start(5000);
    loop.exec();
    check(child.state() == QProcess::NotRunning && child.exitCode() == 0 &&
          ErrorLog::report().contains(QStringLiteral("GPU Canvas fallback (details omitted)")) &&
          !ErrorLog::report().contains(QString::fromLatin1(RemoteGpuSummary)),
          "manager drops GPU summaries delivered after detailed logging is disabled");
    check(!ErrorLog::detailedReport(&detailed, &error), "disabled detailed report unavailable");
    check(ErrorLog::setDetailedEnabled(true, &error), "re-enable detailed capture");
    check(ErrorLog::detailedReport(&detailed, &error) && !detailed.contains(QStringLiteral("detailed secret")),
          "disabled capture clears detailed memory");
    check(ErrorLog::startCoreChannel(&error), "start core channel");
    QByteArray longLine(5000, 'x');
    ErrorLog::sendCoreLine(MEGLER_LOG_ERROR, longLine.constData());
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1500 && !ErrorLog::report().contains(QStringLiteral("events dropped"))) {
        app.processEvents();
        QThread::msleep(10);
    }
    check(ErrorLog::report().contains(QStringLiteral("events dropped")), "oversized core event reported");
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
