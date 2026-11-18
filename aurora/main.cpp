#include <auroraapp.h>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <QtQml>

#include <cstdio>

#include "audio_output.h"
#include "camera_host.h"
#include "bluetooth_host.h"
#include "diagnostic_log.h"
#include "error_log.h"
#include "external_apk_receiver.h"
#include "sensor_host.h"
#include "location_host.h"
#include "side_mouse_buttons.h"
#include "picker_host.h"
#include "install_request_host.h"
#include "standalone_apk_export.h"
#include "network_host.h"
#include "microg_preview_paths.h"
#include "preview.h"
#include "preview_activation_channel.h"
#include "preview_keyboard.h"
#include "probe.h"
#include "standalone_paths.h"
#include "rpm_standalone.h"
#include "rpm_signers.h"
#include "rpm_test_keys.h"

static void MEGLER_CB bundleLog(int, const char *line, void *)
{
    std::fprintf(stderr, "%s\n", line);
}

static void loadTranslations(QCoreApplication *app, QTranslator *translator)
{
    if (QLocale::system().language() != QLocale::Russian) return;
    const QString path = Aurora::Application::pathTo(QStringLiteral("qml/megler_ru.qm")).toLocalFile();
    if (!translator->load(path))
        qWarning().noquote() << "Could not load Russian translations:" << path;
    else
        app->installTranslator(translator);
}

static int unpackBundle(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTranslator translator;
    loadTranslations(&app, &translator);
    bool densityOk = false;
    const int density = QString::fromLocal8Bit(argv[8]).toInt(&densityOk);
    if (!densityOk || density <= 0) {
        std::fprintf(stderr, "Invalid APK bundle density.\n");
        return 2;
    }

    megler_config config = {};
    config.data_dir = argv[3];
    config.package_dir = argv[2];
    config.storage_dir = argv[3];
    config.log = bundleLog;
    if (!megler_init(&config))
        std::fprintf(stderr, "Runtime initialization is unavailable; continuing with APK bundle inspection.\n");

    const QString source = QString::fromLocal8Bit(argv[4]);
    const QString snapshot = QString::fromLocal8Bit(argv[5]);
    if (!QFile::copy(source, snapshot)) {
        std::fprintf(stderr, "Cannot create a private snapshot of the APK bundle.\n");
        return 1;
    }

    char *result = megler_bundle_unpack(argv[5], argv[6], argv[7], density, argv[9], nullptr, nullptr);
    if (!result) {
        std::fprintf(stderr, "Not enough memory to unpack the APK bundle.\n");
        return 1;
    }

    QByteArray resultBytes(result);
    QJsonParseError parseError;
    QJsonDocument bundle = QJsonDocument::fromJson(resultBytes, &parseError);
    if (parseError.error == QJsonParseError::NoError && bundle.isObject() &&
        bundle.object().value(QStringLiteral("ok")).toBool()) {
        const QByteArray apk = QFile::encodeName(QString::fromLocal8Bit(argv[6]));
        const QByteArray splits = QFile::encodeName(QString::fromLocal8Bit(argv[7]));
        char *inspection = megler_apk_report_splits(apk.constData(), splits.constData());
        QJsonParseError inspectionError = {};
        const QJsonDocument report = inspection
            ? QJsonDocument::fromJson(QByteArray(inspection), &inspectionError) : QJsonDocument();
        if (inspection) megler_free(inspection);
        QString error;
        if (inspectionError.error != QJsonParseError::NoError || !report.isObject() ||
            !report.object().value(QStringLiteral("ok")).toBool()) {
            error = report.isObject()
                ? report.object().value(QStringLiteral("error")).toString()
                : QString();
            if (error.isEmpty()) error = QCoreApplication::translate("RpmBuilder", "Could not inspect the unpacked APK bundle.");
        } else if (report.object().value(QStringLiteral("package")).toString() !=
                   bundle.object().value(QStringLiteral("package")).toString()) {
            error = QCoreApplication::translate("RpmBuilder", "The unpacked APK package does not match the bundle.");
        } else if (!report.object().value(QStringLiteral("runnable")).toBool()) {
            error = report.object().value(QStringLiteral("reason")).toString();
            if (error.isEmpty()) error = QCoreApplication::translate("RpmBuilder", "The selected APK bundle cannot be started.");
        }
        if (!error.isEmpty()) {
            QJsonObject failed;
            failed.insert(QStringLiteral("ok"), false);
            failed.insert(QStringLiteral("error"), error);
            resultBytes = QJsonDocument(failed).toJson(QJsonDocument::Compact);
        }
    }
    if (QFileInfo::exists(snapshot) && !QFile::remove(snapshot)) {
        QJsonObject failed;
        failed.insert(QStringLiteral("ok"), false);
        failed.insert(QStringLiteral("error"), QStringLiteral("Could not remove the temporary APK bundle copy."));
        resultBytes = QJsonDocument(failed).toJson(QJsonDocument::Compact);
    }
    std::fputs(resultBytes.constData(), stdout);
    std::fputc('\n', stdout);
    QJsonParseError resultError;
    const QJsonDocument finalResult = QJsonDocument::fromJson(resultBytes, &resultError);
    const bool success = resultError.error == QJsonParseError::NoError && finalResult.isObject() &&
                         finalResult.object().value(QStringLiteral("ok")).toBool();
    megler_free(result);
    return success ? 0 : 1;
}

static int buildStandaloneRpm(const QString &package)
{
    std::fputs("MEGLER_RPM_STAGE:inspect\n", stderr);
    std::fflush(stderr);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString storeRoot = QDir(base).filePath(QStringLiteral("apps"));
    QString error;
    QString output;
    QVariantMap selected;
    if (!base.isEmpty()) {
        for (const QVariant &item : ApkStore(storeRoot).apps()) {
            const QVariantMap app = item.toMap();
            if (app.value(QStringLiteral("package")).toString() == package) {
                selected = app;
                break;
            }
        }
    }
    QImage icon;
    const QString iconData = selected.value(QStringLiteral("icon")).toString();
    const int comma = iconData.indexOf(QLatin1Char(','));
    if (comma >= 0) icon.loadFromData(QByteArray::fromBase64(iconData.mid(comma + 1).toLatin1()));
    if (icon.isNull()) icon.load(QStringLiteral("/usr/share/icons/hicolor/172x172/apps/ru.megler.MeglerProbe.png"));
    bool ok = !base.isEmpty() && !selected.isEmpty();
    if (!ok) {
        error = QCoreApplication::translate("RpmBuilder", "The app or RPM output directory is unavailable.");
    } else {
        std::fputs("MEGLER_RPM_STAGE:build\n", stderr);
        std::fflush(stderr);
        ok = RpmStandalone::createUnsigned(
            package, selected.value(QStringLiteral("label")).toString(), icon,
            storeRoot, Aurora::Application::pathTo(QStringLiteral("android-runtime")).toLocalFile(),
            Aurora::Application::pathTo(QStringLiteral("qml")).toLocalFile(),
            QCoreApplication::applicationFilePath(),
            QDir(base).filePath(QStringLiteral("standalone-rpm-staging")), &output, &error);
    }
    if (ok) {
        std::fputs("MEGLER_RPM_STAGE:sign\n", stderr);
        std::fflush(stderr);
        std::string signingLog;
        ok = RpmSigners::signWithLibrary(output.toUtf8().toStdString(), kTestKeyPem, kTestCertPem, signingLog);
        if (!ok) {
            error = QCoreApplication::translate("RpmBuilder", "Could not sign the standalone RPM: %1")
                .arg(QString::fromStdString(signingLog).trimmed());
            if (!QFile::remove(output))
                error += QCoreApplication::translate("RpmBuilder", " Could not remove the unsigned RPM: %1").arg(output);
        }
    }
    if (ok) {
        std::fputs("MEGLER_RPM_STAGE:save\n", stderr);
        std::fflush(stderr);
        const ApkStore store(storeRoot);
        const QString apkPath = store.apkPath(package);
        QLockFile lock(store.lockPath(package));
        lock.setStaleLockTime(0);
        const QString publishedDir = QDir(QFileInfo(apkPath).absolutePath()).filePath(QStringLiteral("standalone-rpm"));
        const QString published = QDir(publishedDir).filePath(QFileInfo(output).fileName());
        QFile input(output);
        QSaveFile destination(published);
        if (apkPath.isEmpty() || !lock.tryLock(0) ||
            QFileInfo(QFileInfo(apkPath).absolutePath()).isSymLink() || !QFileInfo(apkPath).isFile() ||
            QFileInfo(publishedDir).isSymLink() || !QDir().mkpath(publishedDir) || !input.open(QIODevice::ReadOnly) ||
            !destination.open(QIODevice::WriteOnly)) {
            error = QCoreApplication::translate("RpmBuilder", "Could not publish the signed RPM.");
            ok = false;
        } else {
            char buffer[65536];
            for (;;) {
                const qint64 count = input.read(buffer, sizeof(buffer));
                if (count < 0 || (count > 0 && destination.write(buffer, count) != count)) {
                    error = QCoreApplication::translate("RpmBuilder", "Could not copy the signed RPM to app storage.");
                    ok = false;
                    break;
                }
                if (count == 0) break;
            }
            if (ok && !destination.commit()) {
                error = QCoreApplication::translate("RpmBuilder", "Could not save the signed RPM in app storage.");
                ok = false;
            }
        }
        input.close();
        if (!QFile::remove(output))
            qWarning("Could not remove the temporary signed RPM: %s", qPrintable(output));
        if (ok) output = published;
    }
    QJsonObject result;
    result.insert(QStringLiteral("ok"), ok);
    if (ok) result.insert(QStringLiteral("path"), output);
    else result.insert(QStringLiteral("error"), error);
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fputs(json.constData(), stdout);
    std::fputc('\n', stdout);
    return ok ? 0 : 1;
}

int main(int argc, char *argv[])
{
    const QByteArray mode = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    if (mode == "--unpack-bundle") {
        if (argc != 10) {
            std::fprintf(stderr, "Invalid APK bundle importer arguments.\n");
            return 2;
        }
        return unpackBundle(argc, argv);
    }
    const bool standalone = mode == "--standalone";
    const bool child = mode == "--preview" || standalone || mode == "--build-standalone";
    if (child) DiagnosticLog::installChildQtMessageHandler();
    DiagnosticLog diagnosticLog;
    QString logError;
    if (!child && !diagnosticLog.start(&logError))
        qWarning().noquote() << "In-app log capture unavailable:" << logError;
    if (!child && !diagnosticLog.available()) DiagnosticLog::installChildQtMessageHandler();
    QScopedPointer<QGuiApplication> app(Aurora::Application::application(argc, argv));
    QTranslator translator;
    loadTranslations(app.data(), &translator);
    if (!child && diagnosticLog.available()) diagnosticLog.startNotifications();
    app->setOrganizationName(QStringLiteral("ru.megler"));
    const QString standaloneId = QString::fromLocal8Bit(qgetenv("AURORA_APP_ID"));
    app->setApplicationName(standalone && standaloneId.startsWith(QLatin1String("ru.megler."))
        ? standaloneId.mid(10) : QStringLiteral("MeglerProbe"));
    if (mode != "--build-standalone") {
        QString errorLogError;
        const QString label = standalone ? app->applicationName()
            : mode == "--preview" && argc >= 5 ? QFileInfo(QString::fromLocal8Bit(argv[4])).completeBaseName()
            : QStringLiteral("Android App Manager");
        if (!ErrorLog::initialize(label, &errorLogError))
            qWarning().noquote() << "Error log unavailable:" << errorLogError;
    }

    if (mode == "--build-standalone") {
        if (argc != 3) {
            qWarning("Invalid standalone RPM builder arguments.");
            return 2;
        }
        return buildStandaloneRpm(QString::fromLocal8Bit(argv[2]));
    }

    if (mode == "--preview" && argc != 5 && argc != 7 && argc != 8) {
        qWarning("Invalid microG preview arguments.");
        return 2;
    }

    if (standalone || mode == "--preview") {
        app->setQuitOnLastWindowClosed(false);
        StandalonePaths standalonePathsValue;
        QString pathError;
        if (standalone && (argc != 2 || !standalonePaths(standaloneId,
                QFileInfo(QCoreApplication::applicationFilePath()).fileName(),
                QFileInfo(Aurora::Application::pathTo(QStringLiteral("standalone.json")).toLocalFile()).absolutePath(),
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                &standalonePathsValue, &pathError))) {
            qWarning("Standalone app setup failed: %s", qPrintable(pathError));
            return 1;
        }
        const QString androidRoot = standalone ? standalonePathsValue.androidRoot : QString::fromLocal8Bit(argv[2]);
        if (standalone) {
            app->setApplicationDisplayName(standalonePathsValue.displayName);
            ErrorLog::setApplicationName(standalonePathsValue.displayName);
        }
        const QString data = standalone ? standalonePathsValue.data : QString::fromLocal8Bit(argv[3]);
        const QString apk = standalone ? standalonePathsValue.apk : QString::fromLocal8Bit(argv[4]);
        if (apk.isEmpty()) {
            qWarning("An Android APK is required.");
            return 1;
        }
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString selected = QSettings().value(QStringLiteral("googleServices/gmsApkPath")).toString();
        QString lockPath;
        if (standalone) {
            lockPath = standalonePathsValue.lock;
            if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
                qWarning("Cannot create standalone app storage.");
                return 1;
            }
        } else {
            const QString expectedRoot = Aurora::Application::pathTo(QStringLiteral("android-runtime")).toLocalFile();
            if (QDir::cleanPath(androidRoot) != QDir::cleanPath(expectedRoot) ||
                !microgPreviewPaths(base, expectedRoot, selected, apk, data)) {
                qWarning("Invalid microG preview paths.");
                return 1;
            }
            mg_apk_info info = {};
            const QByteArray apkBytes = apk.toUtf8();
            if (!mg_apk_read_info(apkBytes.constData(), &info) || !mg_apk_gms_name(info.package) ||
                !mg_apk_is_microg(apkBytes.constData()) || info.split[0] || info.config_for_split[0] ||
                info.feature_split || info.split_required || info.required_split_types[0] ||
                !QDir().mkpath(QDir(base).filePath(QStringLiteral("services")))) {
                qWarning("Invalid microG APK or storage directory.");
                return 1;
            }
            lockPath = QDir(base).filePath(QStringLiteral("services/GmsCore.apk.lock"));
        }
        QLockFile appLock(lockPath);
        appLock.setStaleLockTime(0);
        if (!appLock.tryLock(0)) {
            qWarning("Android app data is already in use, or its lock cannot be acquired.");
            return 1;
        }
        if (!QFileInfo(apk).isFile() || !QDir().mkpath(data)) {
            qWarning("The Android app or its data directory is unavailable.");
            return 1;
        }
        PreviewPlayer preview(androidRoot, data, apk);
        if (standalone) {
            preview.setStandaloneSettings(standalonePathsValue.locale, standalonePathsValue.theme,
                                          standalonePathsValue.location);
            preview.setDeniedPermissions(standalonePathsValue.deniedPermissions);
        }
        qmlRegisterType<PreviewSurface>("Megler.Preview", 1, 0, "PreviewSurface");
        QScopedPointer<QQuickView> view(Aurora::Application::createView());
        if (standalone) {
            view->setTitle(standalonePathsValue.displayName);
            view->setResizeMode(QQuickView::SizeRootObjectToView);
        }
        QScopedPointer<PreviewActivationClient> activation;
        if (argc == 8) {
            activation.reset(new PreviewActivationClient(QString::fromLocal8Bit(argv[7]), view.data()));
            QObject::connect(activation.data(), &PreviewActivationClient::channelError,
                             [](const QString &error) { qWarning().noquote() << "Preview activation:" << error; });
            QString activationError;
            if (!activation->start(&activationError))
                qWarning().noquote() << "Preview activation:" << activationError;
        }
        PreviewKeyboard *keyboard = new PreviewKeyboard(view->contentItem());
        preview.setKeyboard(keyboard);
        // The core can retain system callbacks until this process exits.
        AuroraPickerHost *picker = new AuroraPickerHost;
        if (standalone) picker->setDeniedPermissions(standalonePathsValue.deniedPermissions);
        preview.setPicker(picker);
        if (standalone) {
            StandaloneApkExport *exporter = new StandaloneApkExport(data, picker->host());
            if (!exporter->error().isEmpty()) {
                qWarning("APK handoff unavailable: %s", qPrintable(exporter->error()));
                return 1;
            }
            preview.setSystemHost(exporter->host(), exporter->appsDir());
        }
        if (argc >= 7) {
            InstallRequestHost *install = new InstallRequestHost(QString::fromLocal8Bit(argv[5]),
                                                                 QString::fromLocal8Bit(argv[6]), picker->host());
            if (!install->error().isEmpty()) {
                qWarning("Installation host unavailable: %s", qPrintable(install->error()));
                return 1;
            }
            preview.setInstallHost(install);
        }
        view->rootContext()->setContextProperty(QStringLiteral("pickerHost"), picker);
        view->rootContext()->setContextProperty(QStringLiteral("preview"), &preview);
        view->setSource(Aurora::Application::pathTo(QStringLiteral("qml/Preview.qml")));
        if (view->status() == QQuickView::Error) return 1;
        preview.setSystemTheme(view->rootObject()->property("systemDark").toBool(),
                               view->rootObject()->property("systemAccent").value<QColor>());
        // The core's mixer can call the host until the preview process exits.
        AuroraAudioOutput *audio = new AuroraAudioOutput;
        preview.setAudio(audio);
        AuroraNetworkHost network;
        QObject::connect(app.data(), &QGuiApplication::lastWindowClosed, &preview, &PreviewPlayer::close);
        QObject::connect(&preview, &PreviewPlayer::finished, app.data(), &QCoreApplication::quit);
        view->showFullScreen();
        QTimer::singleShot(50, &preview, [&preview]() { preview.start(); });
        const int result = app->exec();
        auroraLocationShutdown();
        network.shutdown();
        picker->stop();
        auroraBluetoothShutdown();
        auroraSensorShutdown();
        auroraCameraShutdown();
        audio->shutdown();
        return result;
    }

    qmlRegisterType<SideMouseButtons>("Megler.Navigation", 1, 0, "SideMouseButtons");
    Probe probe(Aurora::Application::pathTo(QStringLiteral("android-runtime")).toLocalFile(),
                nullptr, &diagnosticLog);
    QScopedPointer<QQuickView> view(Aurora::Application::createView());
    probe.setConfirmationWindow(view.data());
    view->rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
    view->setSource(Aurora::Application::pathTo(QStringLiteral("qml/MeglerProbe.qml")));
    if (view->status() == QQuickView::Error) return 1;
    ExternalApkReceiver fileReceiver;
    QObject::connect(&fileReceiver, &ExternalApkReceiver::requested, &probe, &Probe::requestExternalInstall);
    QObject::connect(&fileReceiver, &ExternalApkReceiver::rejected, &probe, &Probe::externalInstallError);
    QString receiverError;
    if (!fileReceiver.start(&receiverError)) probe.externalInstallError(receiverError);
    view->show();
    const QStringList arguments = app->arguments().mid(1);
    if (!arguments.isEmpty())
        QTimer::singleShot(0, &fileReceiver, [&fileReceiver, arguments]() { fileReceiver.submit(arguments); });
    return app->exec();
}
