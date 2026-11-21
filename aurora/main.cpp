#include <auroraapp.h>
#include <QCoreApplication>
#include <QProcess>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QtQml>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

#include "audio_output.h"
#include "application_executables.h"
#include "camera_host.h"
#include "bluetooth_host.h"
#include "diagnostic_log.h"
#include "error_log.h"
#include "external_apk_receiver.h"
#include "manager_lifecycle.h"
#include "single_instance_lock.h"
#include "library_app_index.h"
#include "sensor_host.h"
#include "location_host.h"
#include "side_mouse_buttons.h"
#include "picker_host.h"
#include "install_request_host.h"
#include "standalone_apk_export.h"
#include "standalone_start.h"
#include "standalone_app_mapping.h"
#include "network_host.h"
#include "microg_preview_paths.h"
#include "apk_store.h"
#include "preview.h"
#include "embedded_preview.h"
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#endif
#include "store_preview_supervisor.h"
#include "store_worker_teardown.h"
#include "preview_activation_channel.h"
#include "preview_keyboard.h"
#include "probe.h"
#include "standalone_paths.h"
#include "standalone_diagnostic_registry.h"
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
#include "standalone_microg_diagnostic.h"
#include "standalone_microg_diagnostic_page.h"
#include <QEventLoop>
#endif
#include "shared_runtime.h"
#include "rpm_standalone.h"
#include "rpm_signers.h"
#include "rpm_test_keys.h"
#include "../core/src/apk/mg_apk.h"
#include "../core/src/zip/mg_zip.h"

static void MEGLER_CB bundleLog(int, const char *line, void *)
{
    std::fprintf(stderr, "%s\n", line);
}

static void loadTranslations(QCoreApplication *app, QTranslator *translator)
{
    if (QLocale::system().language() != QLocale::Russian) return;
    const QString path = Aurora::Application::pathTo(QStringLiteral("qml/androidcompatibilitylayer_ru.qm")).toLocalFile();
    if (!translator->load(path))
        qWarning().noquote() << "Could not load Russian translations:" << path;
    else
        app->installTranslator(translator);
}

static bool prepareSharedMicrogWorker(QString *program, QStringList *arguments,
                                      QString *identityDirectory, QString *error)
{
    if (error) error->clear();
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString androidRoot = Aurora::Application::organizationPathTo(SharedRuntime::location()).toLocalFile();
    if (appData.isEmpty() || androidRoot.isEmpty() || !QFileInfo(androidRoot).isDir()) {
        if (error) *error = QStringLiteral("The shared Android runtime or manager storage is unavailable.");
        return false;
    }

    QSettings settings;
    QString apk = settings.value(QStringLiteral("googleServices/gmsApkPath")).toString();
    if (apk.isEmpty()) apk = QDir(androidRoot).filePath(QStringLiteral("services/GmsCore.apk"));
    const QFileInfo apkInfo(apk);
    if (!apkInfo.isAbsolute() || !apkInfo.isFile() || !apkInfo.isReadable() || apkInfo.isSymLink()) {
        if (error) *error = QStringLiteral("The selected microG APK is unavailable.");
        return false;
    }
    const QByteArray apkBytes = QFile::encodeName(apk);
    mg_apk_info info = {};
    if (!mg_apk_read_info(apkBytes.constData(), &info)) {
        const QString detail = QString::fromUtf8(info.error);
        mg_apk_info_free(&info);
        if (error) *error = detail.isEmpty() ? QStringLiteral("The selected microG APK is invalid.") : detail;
        return false;
    }
    const bool supportedApk = std::strcmp(info.package, "com.google.android.gms") == 0 &&
        mg_apk_is_microg(apkBytes.constData()) && !info.split[0] && !info.config_for_split[0] &&
        !info.feature_split && !info.split_required && !info.required_split_types[0];
    mg_apk_info_free(&info);
    if (!supportedApk) {
        if (error) *error = QStringLiteral("The worker requires a standalone com.google.android.gms microG APK.");
        return false;
    }

    const QString servicesDirectory = QDir(appData).filePath(QStringLiteral("services"));
    const QString dataDirectory = QDir(servicesDirectory).filePath(QStringLiteral("shared-microg-worker"));
    const QString appsDirectory = QDir(appData).filePath(QStringLiteral("apps"));
    if (!QDir().mkpath(dataDirectory) || !QDir().mkpath(appsDirectory) ||
        QFileInfo(appData).isSymLink() || QFileInfo(servicesDirectory).isSymLink() ||
        QFileInfo(dataDirectory).isSymLink() || QFileInfo(appsDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("The shared microG storage directories are unavailable or unsafe.");
        return false;
    }

    const QString executable = ApplicationExecutables::worker();
    const QFileInfo executableInfo(executable);
    if (!executableInfo.isAbsolute() || !executableInfo.isFile() || !executableInfo.isExecutable() ||
        executableInfo.isSymLink()) {
        if (error) *error = QStringLiteral("The manager worker executable is unavailable.");
        return false;
    }
    *program = executableInfo.absoluteFilePath();
    *arguments = QStringList()
        << QStringLiteral("--apk=") + apkInfo.absoluteFilePath()
        << QStringLiteral("--data-dir=") + QDir(dataDirectory).absolutePath()
        << QStringLiteral("--android-root=") + QFileInfo(androidRoot).absoluteFilePath()
        << QStringLiteral("--apps-dir=") + QDir(appsDirectory).absolutePath();
    *identityDirectory = appData;
    return true;
}

static bool validateStoreBundle(const QString &path, int *splitCount, QString *error)
{
    const QByteArray source = QFile::encodeName(path);
    char why[256] = {};
    mg_zip zip;
    if (!mg_zip_open_limited(&zip, source.constData(), 32, 2u * 1024 * 1024, why, sizeof(why))) {
        *error = QString::fromUtf8(why);
        return false;
    }
    QTemporaryDir temporary(QFileInfo(path).absolutePath() + QStringLiteral("/.verify-XXXXXX"));
    bool ok = temporary.isValid() && zip.count >= 2 && zip.count <= 11;
    quint64 total = 0;
    QString package;
    int version = 0;
    int bases = 0;
    QSet<QString> names;
    QSet<QString> splits;
    const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9._-]{1,200}\\.apk$"),
                                      QRegularExpression::CaseInsensitiveOption);
    for (u32 i = 0; ok && i < zip.count; ++i) {
        const mg_zip_entry &entry = zip.entries[i];
        const QString name = QString::fromLatin1(entry.name);
        if (!safeName.match(name).hasMatch() || names.contains(name) ||
            entry.usize > 512u * 1024 * 1024 ||
            entry.usize > 512u * 1024 * 1024 - total) {
            ok = false;
            break;
        }
        names.insert(name);
        total += entry.usize;
        const QString part = temporary.path() + QLatin1Char('/') + QString::number(i) + QStringLiteral(".apk");
        const QByteArray partName = QFile::encodeName(part);
        if (!mg_zip_extract(&zip, &entry, partName.constData(), nullptr, nullptr, why, sizeof(why))) {
            ok = false;
            break;
        }
        mg_apk_info info;
        if (!mg_apk_read_info(partName.constData(), &info) || !info.package[0]) {
            ok = false;
            break;
        }
        const QString partPackage = QString::fromUtf8(info.package);
        if (package.isEmpty()) {
            package = partPackage;
            version = info.version_code;
        } else if (package != partPackage || version != info.version_code) {
            ok = false;
            break;
        }
        const QString split = QString::fromUtf8(info.split);
        if (split.isEmpty()) ++bases;
        else if (splits.contains(split)) ok = false;
        else splits.insert(split);
        if (!QFile::remove(part)) ok = false;
    }
    mg_zip_close(&zip);
    if (!ok || bases != 1 || splits.isEmpty()) {
        *error = QCoreApplication::translate("RpmBuilder",
            "The store bundle is invalid, too large, or its APK packages or versions do not match.");
        return false;
    }
    *splitCount = splits.size();
    return true;
}

static bool safeStoreExpansionPath(QString path, const QString &package, bool directory = false)
{
    if (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    if (path.endsWith(QLatin1Char('/'))) path.chop(1);
    const QStringList parts = path.split(QLatin1Char('/'));
    static const QRegularExpression safePart(QStringLiteral("^[A-Za-z0-9._-]{1,200}$"));
    if (parts.isEmpty() || parts.at(0) != QLatin1String("Android") ||
        (!directory && parts.size() < 4) || (directory && parts.size() < 1)) return false;
    if (parts.size() == 1) return directory;
    if (parts.at(1) != QLatin1String("obb") && parts.at(1) != QLatin1String("data")) return false;
    if (parts.size() == 2) return directory;
    if (!package.isEmpty() && parts.at(2) != package) return false;
    if (!directory && parts.size() < 4) return false;
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..") || !safePart.match(part).hasMatch())
            return false;
    }
    return true;
}

static bool validateStoreXapk(const QString &path, QString *package, int *expansionCount, QString *error)
{
    const QByteArray source = QFile::encodeName(path);
    char why[256] = {};
    mg_zip zip;
    if (!mg_zip_open_limited(&zip, source.constData(), 256, 16u * 1024 * 1024, why, sizeof(why))) {
        *error = QString::fromUtf8(why);
        return false;
    }
    QTemporaryDir temporary(QFileInfo(path).absolutePath() + QStringLiteral("/.verify-XXXXXX"));
    bool ok = temporary.isValid() && zip.count > 0;
    quint64 total = 0;
    int apkCount = 0;
    int bases = 0;
    int version = 0;
    QString parsedPackage;
    QString manifestPackage;
    QJsonObject manifest;
    bool sawManifest = false;
    bool sawIcon = false;
    QSet<QString> names;
    QSet<QString> splits;
    QSet<QString> expansionEntries;
    QStringList expansionDirectories;
    const QRegularExpression safeApkName(QStringLiteral("^[A-Za-z0-9._-]{1,200}\\.apk$"),
                                         QRegularExpression::CaseInsensitiveOption);
    for (u32 i = 0; ok && i < zip.count; ++i) {
        const mg_zip_entry &entry = zip.entries[i];
        const QString name = QString::fromLatin1(entry.name);
        const QByteArray encodedName = name.toLatin1();
        if (encodedName.size() != static_cast<int>(std::strlen(entry.name)) ||
            names.contains(name) || (entry.flags & 1) ||
            (entry.method != 0 && entry.method != 8) ||
            entry.usize > 512u * 1024 * 1024 ||
            entry.usize > 512u * 1024 * 1024 - total) {
            ok = false;
            break;
        }
        names.insert(name);
        total += entry.usize;
        if (name.endsWith(QLatin1Char('/'))) {
            if (!safeStoreExpansionPath(name, QString(), true))
                ok = false;
            else expansionDirectories.append(name);
            continue;
        }
        if (name.endsWith(QLatin1String(".apk"), Qt::CaseInsensitive)) {
            if (!safeApkName.match(name).hasMatch() || ++apkCount > 11) {
                ok = false;
                break;
            }
            const QString part = temporary.path() + QLatin1Char('/') + QString::number(i) + QStringLiteral(".apk");
            const QByteArray partName = QFile::encodeName(part);
            if (!mg_zip_extract(&zip, &entry, partName.constData(), nullptr, nullptr, why, sizeof(why))) {
                ok = false;
                break;
            }
            mg_apk_info info;
            if (!mg_apk_read_info(partName.constData(), &info) || !info.package[0]) {
                ok = false;
                break;
            }
            const QString partPackage = QString::fromUtf8(info.package);
            if (parsedPackage.isEmpty()) {
                parsedPackage = partPackage;
                version = info.version_code;
            } else if (parsedPackage != partPackage || version != info.version_code) {
                ok = false;
                break;
            }
            const QString split = QString::fromUtf8(info.split);
            if (split.isEmpty()) ++bases;
            else if (splits.contains(split)) ok = false;
            else splits.insert(split);
            if (!QFile::remove(part)) ok = false;
        } else if (name == QLatin1String("manifest.json")) {
            if (sawManifest || entry.usize > 4u * 1024 * 1024) {
                ok = false;
                break;
            }
            sawManifest = true;
            u8 *bytes = mg_zip_read(&zip, &entry, 4u * 1024 * 1024, nullptr);
            if (!bytes) { ok = false; break; }
            const QByteArray contents(reinterpret_cast<const char *>(bytes), static_cast<int>(entry.usize));
            free(bytes);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                ok = false;
                break;
            }
            manifest = document.object();
            if (manifest.contains(QStringLiteral("package_name"))) {
                if (!manifest.value(QStringLiteral("package_name")).isString()) {
                    ok = false;
                    break;
                }
                manifestPackage = manifest.value(QStringLiteral("package_name")).toString();
                if (manifestPackage.isEmpty()) {
                    ok = false;
                    break;
                }
            }
        } else if (name == QLatin1String("icon.png")) {
            if (sawIcon || entry.usize > 4u * 1024 * 1024) ok = false;
            sawIcon = true;
        } else if (name.startsWith(QLatin1String("Android/obb/")) ||
                   name.startsWith(QLatin1String("Android/data/"))) {
            expansionEntries.insert(name);
        } else {
            ok = false;
        }
    }
    mg_zip_close(&zip);

    QSet<QString> expansions = expansionEntries;
    if (ok && sawManifest && manifest.contains(QStringLiteral("expansions"))) {
        const QJsonValue value = manifest.value(QStringLiteral("expansions"));
        if (!value.isArray()) ok = false;
        else {
            const QJsonArray list = value.toArray();
            for (const QJsonValue &item : list) {
                if (!item.isObject()) { ok = false; break; }
                const QJsonObject expansion = item.toObject();
                const QJsonValue fileValue = expansion.value(QStringLiteral("file"));
                const QJsonValue installValue = expansion.value(QStringLiteral("install_path"));
                if (!fileValue.isString() || !installValue.isString() ||
                    !safeStoreExpansionPath(installValue.toString(), parsedPackage) ||
                    !expansionEntries.contains(fileValue.toString())) {
                    ok = false;
                    break;
                }
                expansions.insert(fileValue.toString());
            }
        }
    }
    if (manifestPackage != QString() && manifestPackage != parsedPackage) ok = false;
    for (const QString &directory : expansionDirectories)
        if (!safeStoreExpansionPath(directory, parsedPackage, true)) ok = false;
    for (const QString &entry : expansionEntries)
        if (!safeStoreExpansionPath(entry, parsedPackage)) ok = false;
    static const QRegularExpression safePackage(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    if (apkCount == 0 || bases != 1 || parsedPackage.size() > 159 || !safePackage.match(parsedPackage).hasMatch() ||
        (!manifestPackage.isEmpty() && manifestPackage != parsedPackage) ||
        expansions.size() > 16) ok = false;
    if (!ok) {
        *error = QCoreApplication::translate("RpmBuilder",
            "The XAPK is invalid, too large, or contains APKs or expansion files that do not match.");
        return false;
    }
    *package = parsedPackage;
    *expansionCount = expansions.size();
    return true;
}

static bool limitStoreImporter()
{
    const struct { int resource; rlim_t maximum; } limits[] = {
        {RLIMIT_AS, 768u * 1024 * 1024},
        {RLIMIT_FSIZE, 512u * 1024 * 1024},
        {RLIMIT_CPU, 180}
    };
    for (const auto &limit : limits) {
        struct rlimit current;
        if (getrlimit(limit.resource, &current) != 0) return false;
        if (current.rlim_cur > limit.maximum) current.rlim_cur = limit.maximum;
        if (setrlimit(limit.resource, &current) != 0) return false;
    }
    return true;
}

static int unpackBundle(int argc, char *argv[], bool storeBundle, bool storeXapk)
{
    if ((storeBundle || storeXapk) && !limitStoreImporter()) {
        std::fprintf(stderr, "Cannot limit APK bundle importer resources.\n");
        return 1;
    }
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
    int expectedSplits = 0;
    int expectedExpansions = 0;
    QString expectedPackage;
    if (storeBundle) {
        QString error;
        if (!validateStoreBundle(snapshot, &expectedSplits, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
    } else if (storeXapk) {
        QString error;
        if (!validateStoreXapk(snapshot, &expectedPackage, &expectedExpansions, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
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
        if (storeBundle && bundle.object().value(QStringLiteral("splits")).toArray().size() != expectedSplits) {
            error = QCoreApplication::translate("RpmBuilder", "The runtime cannot retain every split APK in this session.");
        } else if (storeXapk && bundle.object().value(QStringLiteral("package")).toString() != expectedPackage) {
            error = QCoreApplication::translate("RpmBuilder", "The unpacked XAPK package does not match the archive.");
        } else if (storeXapk) {
            const QJsonArray expansions = bundle.object().value(QStringLiteral("expansions")).toArray();
            int copiedExpansions = 0;
            for (const QJsonValue &item : expansions) {
                if (!item.isObject() ||
                    !item.toObject().value(QStringLiteral("to")).isString() ||
                    item.toObject().value(QStringLiteral("to")).toString().isEmpty()) {
                    error = QCoreApplication::translate("RpmBuilder", "An XAPK expansion file could not be placed safely.");
                    break;
                }
                ++copiedExpansions;
            }
            if (error.isEmpty() && copiedExpansions != expectedExpansions)
                error = QCoreApplication::translate("RpmBuilder", "Not every XAPK expansion file could be imported.");
        } else if (inspectionError.error != QJsonParseError::NoError || !report.isObject() ||
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

static int buildStandaloneRpm(const QString &package, bool fileDiagnostics)
{
    std::fputs("ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:inspect\n", stderr);
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
    if (icon.isNull()) icon.load(QStringLiteral("/usr/share/icons/hicolor/172x172/apps/ru.androidcompatibilitylayer.appmanager.png"));
    const QString label = selected.value(QStringLiteral("label")).toString();
    const QString runtimeRoot = Aurora::Application::organizationPathTo(SharedRuntime::location()).toLocalFile();
    const QString qmlRoot = Aurora::Application::pathTo(QStringLiteral("qml")).toLocalFile();
    const QString executable = ApplicationExecutables::standalonePayload(QFileInfo(qmlRoot).absolutePath());
    const ApkStore store(storeRoot);
    const QString apkPath = store.apkPath(package);
    const QString publishedDir = QDir(QFileInfo(apkPath).absolutePath()).filePath(QStringLiteral("standalone-rpm"));
    QByteArray fingerprint;
    bool cacheHit = false;
    bool ok = !base.isEmpty() && !selected.isEmpty();
    if (!ok) {
        error = QCoreApplication::translate("RpmBuilder", "The app or RPM output directory is unavailable.");
    } else {
        ok = RpmStandalone::reuseOrGenerate([&]() {
            QLockFile lock(store.lockPath(package));
            lock.setStaleLockTime(0);
            if (!lock.tryLock(0)) return QString();
            fingerprint = RpmStandalone::cacheFingerprint(package, label, icon, storeRoot, runtimeRoot,
                                                            qmlRoot, executable, fileDiagnostics,
                                                            QStringLiteral(ACL_MANAGER_BUILD_VERSION));
            return RpmStandalone::cachedRpm(publishedDir, fingerprint);
        }, [&](QString *path) {
            std::fputs("ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:build\n", stderr);
            std::fflush(stderr);
            return RpmStandalone::createUnsigned(
                package, label, icon, storeRoot, runtimeRoot, qmlRoot, executable,
                QDir(base).filePath(QStringLiteral("standalone-rpm-staging")), path, &error,
                fileDiagnostics);
        }, &output, &cacheHit);
        if (cacheHit) qInfo("Standalone RPM: using cached package");
    }
    if (ok && !cacheHit) {
        std::fputs("ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:sign\n", stderr);
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
    if (ok && !cacheHit) {
        std::fputs("ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:save\n", stderr);
        std::fflush(stderr);
        QLockFile lock(store.lockPath(package));
        lock.setStaleLockTime(0);
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
            if (ok) {
                const QString previous = StandaloneAppMapping::read(publishedDir);
                if (!StandaloneAppMapping::save(publishedDir, published)) {
                    error = QCoreApplication::translate("RpmBuilder", "Could not save the standalone app mapping.");
                    ok = false;
                } else if (!fingerprint.isEmpty() &&
                           fingerprint == RpmStandalone::cacheFingerprint(package, label, icon, storeRoot,
                                                                           runtimeRoot, qmlRoot, executable,
                                                                           fileDiagnostics,
                                                                           QStringLiteral(ACL_MANAGER_BUILD_VERSION)) &&
                           !RpmStandalone::saveCache(publishedDir, published, fingerprint)) {
                    qWarning("Standalone RPM: could not save cache metadata");
                }
                if (ok && !previous.isEmpty()) {
                    for (const QString &arch : {QStringLiteral("aarch64"), QStringLiteral("x86_64")}) {
                        const QString old = QDir(publishedDir).filePath(previous + QStringLiteral("-1.0.0-1.") +
                                                                        arch + QStringLiteral(".rpm"));
                        if (old != published && QFileInfo(old).exists() && !QFile::remove(old))
                            qWarning("Standalone RPM: could not remove obsolete cached package");
                    }
                }
            }
        }
        input.close();
        if (!QFile::remove(output))
            qWarning("Could not remove the temporary signed RPM: %s", qPrintable(output));
        if (ok) output = published;
    }
    if (ok && !StandaloneDiagnosticRegistry::save(base, output, package))
        qWarning("Standalone identity: could not register exported app; caller identification will be denied.");
    QJsonObject result;
    result.insert(QStringLiteral("ok"), ok);
    if (ok) result.insert(QStringLiteral("path"), output);
    else result.insert(QStringLiteral("error"), error);
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fputs(json.constData(), stdout);
    std::fputc('\n', stdout);
    return ok ? 0 : 1;
}

namespace {
// Declare each marker before the objects whose destruction it confirms.
struct StoreWorkerTeardownMarker {
    const bool &enabled;
    const char *stage;

    ~StoreWorkerTeardownMarker()
    {
        if (enabled) traceStoreWorkerTeardown(stage);
    }
};
}

int runApplication(int argc, char *argv[], void (*initReportWebView)())
{
    const QByteArray mode = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    if (mode == "--gpu-probe") {
        if (argc != 2) {
            std::fprintf(stderr, "Usage: %s --gpu-probe\n", argv[0]);
            return 2;
        }
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
        QScopedPointer<QGuiApplication> probeApp(Aurora::Application::application(argc, argv));
        QOpenGLContext context;
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(2, 0);
        context.setFormat(format);
        if (!context.create() || !context.isOpenGLES()) {
            std::fprintf(stderr, "GPU probe: could not create a Qt GLES context.\n");
            return 1;
        }
        QOffscreenSurface surface(context.screen());
        surface.setFormat(context.format());
        surface.create();
        if (!surface.isValid() || !context.makeCurrent(&surface)) {
            std::fprintf(stderr, "GPU probe: could not bind the Qt GLES context.\n");
            return 1;
        }
        for (const QString &line : EmbeddedGpuTransport::currentGpuCapabilities())
            std::puts(qPrintable(line));
        context.doneCurrent();
        return 0;
#else
        std::puts("GPU probe unavailable: build has no experimental GPU composition. Aurora Store uses CPU Canvas.");
        return 1;
#endif
    }
    if (mode == "--unpack-bundle" || mode == "--unpack-store-bundle" || mode == "--unpack-store-xapk") {
        if (argc != 10) {
            std::fprintf(stderr, "Invalid APK bundle importer arguments.\n");
            return 2;
        }
        return unpackBundle(argc, argv, mode == "--unpack-store-bundle", mode == "--unpack-store-xapk");
    }
    const bool standalone = mode == "--standalone";
    const bool child = mode == "--preview" || standalone || mode == "--build-standalone";
    const bool backgroundManager = mode == "--microg-service";
    if (backgroundManager && argc != 2) {
        std::fprintf(stderr, "Invalid background manager arguments.\n");
        return 2;
    }
    // A D-Bus-started manager can later show the same UI, including the WebView report form.
    if (!child)
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    bool traceStoreWorkerExit = false;
    const StoreWorkerTeardownMarker appDestroyed{traceStoreWorkerExit, "Qt application destroyed; leaving main"};
    QScopedPointer<QGuiApplication> app;
    if (child) DiagnosticLog::installChildQtMessageHandler();
    const StoreWorkerTeardownMarker logDestroyed{traceStoreWorkerExit, "diagnostic capture destroyed"};
    DiagnosticLog diagnosticLog;
    const StoreWorkerTeardownMarker previewScopeExited{traceStoreWorkerExit,
        "preview scope and main locals destroyed; destroying diagnostic capture"};
    QString logError;
    if (!child && !diagnosticLog.start(&logError, true))
        qWarning().noquote() << "In-app log capture unavailable:" << logError;
    if (!child && !diagnosticLog.available()) DiagnosticLog::installChildQtMessageHandler();
    app.reset(Aurora::Application::application(argc, argv));
    QTranslator translator;
    loadTranslations(app.data(), &translator);
    if (!child && diagnosticLog.available()) diagnosticLog.startNotifications();
    app->setOrganizationName(QStringLiteral("ru.androidcompatibilitylayer"));
    const QString standaloneId = QString::fromLocal8Bit(qgetenv("AURORA_APP_ID"));
    const QString standalonePrefix = QStringLiteral("ru.androidcompatibilitylayer.");
    app->setApplicationName(standalone && standaloneId.startsWith(standalonePrefix)
        ? standaloneId.mid(standalonePrefix.size()) : QStringLiteral("appmanager"));
    const QString sharedLogRoot = Aurora::Application::organizationFilesDir(false).path();
    if (mode != "--build-standalone") {
        QString errorLogError;
        const QString label = standalone ? app->applicationName()
            : mode == "--preview" && argc >= 5 ? QFileInfo(QString::fromLocal8Bit(argv[4])).completeBaseName()
            : QStringLiteral("Android App Manager");
        if (!ErrorLog::initialize(label, &errorLogError))
            qWarning().noquote() << "Error log unavailable:" << errorLogError;
    }
    StandalonePaths standalonePathsValue;
    QString standaloneFilePath;
    if (standalone) {
        QString pathError;
        StandaloneRuntimeError runtimeError = StandaloneRuntimeError::None;
        if (argc != 2 || !standalonePaths(standaloneId,
                QFileInfo(QCoreApplication::applicationFilePath()).fileName(),
                QFileInfo(Aurora::Application::pathTo(QStringLiteral("standalone.json")).toLocalFile()).absolutePath(),
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                &standalonePathsValue, &pathError,
                Aurora::Application::organizationPathTo(SharedRuntime::location()).toLocalFile(),
                &runtimeError)) {
            qWarning("Standalone app setup failed: %s", qPrintable(pathError));
            if (runtimeError != StandaloneRuntimeError::None) {
                QScopedPointer<QQuickView> errorView(Aurora::Application::createView());
                errorView->setResizeMode(QQuickView::SizeRootObjectToView);
                QObject::connect(errorView->engine(), &QQmlEngine::quit, app.data(), &QCoreApplication::quit);
                errorView->rootContext()->setContextProperty(QStringLiteral("runtimeIncompatible"),
                    runtimeError == StandaloneRuntimeError::Incompatible);
                errorView->setSource(Aurora::Application::pathTo(QStringLiteral("qml/StandaloneRuntimeError.qml")));
                if (errorView->status() == QQuickView::Error) return 1;
                errorView->showFullScreen();
                return app->exec();
            }
            return 1;
        }
        if (standalonePathsValue.standaloneSnapshots) {
            QString error;
            if (!ErrorLog::startStandaloneSnapshots(sharedLogRoot, standaloneId, &error))
                qWarning().noquote() << "Standalone log snapshots unavailable:" << error;
        } else if (standalonePathsValue.fileDiagnostics) {
            QString error;
            if (!ErrorLog::startStandaloneDiagnosticFile(&standaloneFilePath, &error))
                qWarning().noquote() << "Standalone diagnostic file unavailable:" << error;
        }
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
        const StandaloneMicrogDiagnostic::Result diagnostic =
            StandaloneMicrogDiagnostic::run(QDBusConnection::sessionBus());
        app->setQuitOnLastWindowClosed(false);
        QScopedPointer<QQuickView> diagnosticView(Aurora::Application::createView());
        diagnosticView->setResizeMode(QQuickView::SizeRootObjectToView);
        diagnosticView->rootContext()->setContextProperty(QStringLiteral("ownerStatus"), diagnostic.owner);
        diagnosticView->rootContext()->setContextProperty(QStringLiteral("replyStatus"), diagnostic.response);
        QQmlComponent *diagnosticPage = StandaloneMicrogDiagnostic::createPage(diagnosticView.data());
        QObject *page = diagnosticPage->create(diagnosticView->rootContext());
        if (!page) {
            qWarning("Standalone diagnostic page could not be created: %s", qPrintable(diagnosticPage->errorString()));
        } else {
            diagnosticView->setContent(QUrl(), diagnosticPage, page);
            QEventLoop diagnosticLoop;
            QObject::connect(page, SIGNAL(continueRequested()), &diagnosticLoop, SLOT(quit()));
            QObject::connect(app.data(), &QGuiApplication::lastWindowClosed, &diagnosticLoop, &QEventLoop::quit);
            diagnosticView->showFullScreen();
            diagnosticLoop.exec();
        }
#endif
    }
    if (standalone || mode == "--preview") {
        QString error;
        if (!ErrorLog::startForwarding(&error))
            qWarning().noquote() << "Shared in-memory log unavailable:" << error;
        if (!diagnosticLog.start(&error, true)) {
            qWarning().noquote() << "Detailed process output unavailable:" << error;
        }
    }

    if (mode == "--build-standalone") {
        if (argc != 3 && !(argc == 4 && QByteArray(argv[3]) == "--file-diagnostics")) {
            qWarning("Invalid standalone RPM builder arguments.");
            return 2;
        }
        return buildStandaloneRpm(QString::fromLocal8Bit(argv[2]), argc == 4);
    }

    if (mode == "--preview" && argc != 5 && argc != 7 && argc != 8 && argc != 9) {
        qWarning("Invalid microG preview arguments.");
        return 2;
    }

    if (standalone || mode == "--preview") {
        app->setQuitOnLastWindowClosed(false);
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
        bool storePreview = false;
        if (standalone) {
            lockPath = standalonePathsValue.lock;
            if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
                qWarning("Cannot create standalone app storage.");
                return 1;
            }
        } else {
            const QString expectedRoot = Aurora::Application::organizationPathTo(SharedRuntime::location()).toLocalFile();
            const ApkStore store(QDir(base).filePath(QStringLiteral("apps")));
            const QString package = QStringLiteral("com.aurora.store");
            storePreview = !base.isEmpty() &&
                QDir::cleanPath(apk) == QDir::cleanPath(store.apkPath(package)) &&
                QDir::cleanPath(data) == QDir::cleanPath(store.dataPath(package));
            if (QDir::cleanPath(androidRoot) != QDir::cleanPath(expectedRoot) ||
                (!storePreview && !microgPreviewPaths(base, expectedRoot, selected, apk, data))) {
                qWarning("Invalid preview paths.");
                return 1;
            }
            mg_apk_info info = {};
            const QByteArray apkBytes = apk.toUtf8();
            if (!mg_apk_read_info(apkBytes.constData(), &info) ||
                (storePreview ? (QFileInfo(QFileInfo(apk).absolutePath()).isSymLink() ||
                                 QFileInfo(apk).isSymLink() || QFileInfo(data).isSymLink() ||
                                 QString::fromUtf8(info.package) != package)
                              : (!mg_apk_gms_name(info.package) || !mg_apk_is_microg(apkBytes.constData()))) ||
                info.split[0] || info.config_for_split[0] ||
                info.feature_split || info.split_required || info.required_split_types[0] ||
                (!storePreview && !QDir().mkpath(QDir(base).filePath(QStringLiteral("services"))))) {
                qWarning("Invalid preview APK or storage directory.");
                return 1;
            }
            lockPath = storePreview ? store.lockPath(package)
                                    : QDir(base).filePath(QStringLiteral("services/GmsCore.apk.lock"));
        }
        const bool embedded = !standalone && argc == 9;
        QLockFile appLock(lockPath);
        appLock.setStaleLockTime(0);
        qint64 lockOwner = 0;
        const bool parentOwnsStoreLock = storePreview && embedded &&
                appLock.getLockInfo(&lockOwner, nullptr, nullptr) && lockOwner == getppid();
        if (parentOwnsStoreLock && (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != lockOwner)) {
            qWarning("Cannot bind Aurora Store to its window process.");
            return 1;
        }
        if (!parentOwnsStoreLock && !appLock.tryLock(0)) {
            qWarning("Android app data is already in use, or its lock cannot be acquired.");
            return 1;
        }
        if (!QFileInfo(apk).isFile() || !QDir().mkpath(data)) {
            qWarning("The Android app or its data directory is unavailable.");
            return 1;
        }
        if (storePreview && !embedded) {
            qmlRegisterType<PreviewSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
            EmbeddedPreview remote;
            remote.setCpuFrameTransportOnly(qEnvironmentVariableIntValue("ACL_EMBEDDED_GPU_GBM_VERIFY_PARENT") == 1);
            QScopedPointer<QQuickView> view(Aurora::Application::createView());
            view->setTitle(QStringLiteral("Aurora Store"));
            view->setResizeMode(QQuickView::SizeRootObjectToView);
            remote.setCloseOnBackGesture(true);
            PreviewKeyboard *keyboard = new PreviewKeyboard(view->contentItem());
            remote.setRemoteKeyboard(keyboard);
            QScopedPointer<PreviewActivationClient> activation;
            if (argc == 8) {
                activation.reset(new PreviewActivationClient(QString::fromLocal8Bit(argv[7]), view.data()));
                QObject::connect(activation.data(), &PreviewActivationClient::channelError,
                                 [](const QString &error) { qWarning().noquote() << "Preview activation:" << error; });
                QString error;
                if (!activation->start(&error)) qWarning().noquote() << "Preview activation:" << error;
            }
            QStringList args;
            for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
            while (args.size() < 7) args << QString();
            StorePreviewSupervisor supervisor(&remote, QCoreApplication::applicationFilePath(), args);
            QObject::connect(&remote, &PreviewPlayer::returnToManagerRequested, []() {
                QString error;
                const ManagerLifecycle::ForwardResult result =
                    ManagerLifecycle::forwardToRunningManager(QStringList(), &error);
                const char *outcome = result == ManagerLifecycle::Delivered ? "delivered"
                    : result == ManagerLifecycle::NoOwner ? "no-owner"
                    : result == ManagerLifecycle::Unavailable ? "unavailable" : "failed";
                qInfo("Aurora Store return to manager: activation=%s.", outcome);
                if (result == ManagerLifecycle::NoOwner) {
                    if (!QProcess::startDetached(ApplicationExecutables::manager(), QStringList()))
                        qWarning("Cannot reopen the manager.");
                } else if (result != ManagerLifecycle::Delivered) {
                    qWarning().noquote() << "Cannot activate the manager:" << error;
                }
            });
            QObject::connect(&remote, &EmbeddedPreview::channelError,
                             [](const QString &error) { qWarning().noquote() << "Store preview:" << error; });
            QObject::connect(app.data(), &QGuiApplication::lastWindowClosed, &supervisor, [&supervisor]() {
                qInfo("Aurora Store window closed.");
                supervisor.stop();
            });
            QObject::connect(&supervisor, &StorePreviewSupervisor::finished, app.data(),
                             [&app](int code, bool crashed) { app->exit(crashed ? 1 : code); });
            view->rootContext()->setContextProperty(QStringLiteral("pickerHost"), &remote);
            view->rootContext()->setContextProperty(QStringLiteral("preview"), &remote);
            view->rootContext()->setContextProperty(QStringLiteral("standaloneFileDiagnostics"), false);
            view->rootContext()->setContextProperty(QStringLiteral("diagnosticLog"), &diagnosticLog);
            view->setSource(Aurora::Application::pathTo(QStringLiteral("qml/Preview.qml")));
            if (view->status() == QQuickView::Error) return 1;
            remote.setRemoteTheme(view->rootObject()->property("systemDark").toBool(),
                                  view->rootObject()->property("systemAccent").value<QColor>());
            QString error;
            if (!supervisor.start(&error)) {
                qWarning().noquote() << error;
                return 1;
            }
            view->showFullScreen();
            const int result = app->exec();
            view.reset();
            return result;
        }
        const StoreWorkerTeardownMarker previewDestroyed{traceStoreWorkerExit, "preview player destroyed"};
        PreviewPlayer preview(androidRoot, data, apk);
        preview.setCloseOnExit(standalone || storePreview);
        if (embedded) {
            preview.setCpuPresentationOnly(true);
            AuroraPickerHost *picker = new AuroraPickerHost;
            preview.setPicker(picker);
            if (storePreview) {
                StandaloneApkExport *exporter = new StandaloneApkExport(data, sharedLogRoot, picker->host(), nullptr,
                    QDir(data).filePath(QStringLiteral(".manager-library-cache")), StandaloneApkExport::QueueLaunch);
                if (!exporter->error().isEmpty()) {
                    qWarning("APK handoff unavailable: %s", qPrintable(exporter->error()));
                    return 1;
                }
                preview.setSystemHost(exporter->host(), exporter->appsDir());
            } else {
                InstallRequestHost *install = new InstallRequestHost(QString::fromLocal8Bit(argv[5]),
                                                                     QString::fromLocal8Bit(argv[6]), picker->host());
                if (!install->error().isEmpty()) {
                    qWarning("Installation host unavailable: %s", qPrintable(install->error()));
                    return 1;
                }
                preview.setInstallHost(install);
            }
            EmbeddedPreviewChild *bridge = new EmbeddedPreviewChild(&preview, picker);
            preview.setRemoteKeyboard(bridge->keyboardHost());
            QString channelError;
            if (!bridge->connectTo(QString::fromLocal8Bit(argv[8]), &channelError)) {
                qWarning().noquote() << channelError;
                return 1;
            }
            QObject::connect(bridge, &EmbeddedPreviewChild::failed, &preview, [&preview](const QString &error) {
                qWarning().noquote() << "Embedded preview:" << error;
                preview.close();
            });
            bool androidExited = false;
            if (storePreview) {
                QObject::connect(&preview, &PreviewPlayer::finished, app.data(),
                                 [&app, &preview, &androidExited]() {
                    androidExited = true;
                    qInfo("Aurora Store worker: Android exit delivered, status=%d.", preview.exitStatus());
                    app->exit(preview.exitStatus());
                });
            } else {
                QObject::connect(&preview, &PreviewPlayer::finished, app.data(), &QCoreApplication::quit);
            }
            AuroraAudioOutput *audio = new AuroraAudioOutput;
            preview.setAudio(audio);
            const StoreWorkerTeardownMarker networkDestroyed{traceStoreWorkerExit, "network host destroyed"};
            AuroraNetworkHost network;
            const int result = app->exec();
            if (storePreview) qInfo("Aurora Store worker: event loop exited, shutting down hosts.");
            auroraLocationShutdown();
            network.shutdown();
            picker->stop();
            auroraBluetoothShutdown();
            auroraSensorShutdown();
            auroraCameraShutdown();
            audio->shutdown();
            if (storePreview) {
                traceStoreWorkerExit = true;
                preview.enableStoreWorkerTeardownTrace();
                qInfo("Aurora Store worker: host shutdown complete, status=%d.", result);
                if (androidExited && result == 0) bridge->notifyShutdownComplete();
            }
            return result;
        }
        preview.setCloseOnBackGesture(!standalone);
        if (!standalone) {
            QObject::connect(&preview, &PreviewPlayer::returnToManagerRequested, []() {
                QString error;
                const ManagerLifecycle::ForwardResult result =
                    ManagerLifecycle::forwardToRunningManager(QStringList(), &error);
                if (result == ManagerLifecycle::NoOwner) {
                    if (!QProcess::startDetached(ApplicationExecutables::manager(), QStringList()))
                        qWarning("Cannot reopen the manager.");
                } else if (result != ManagerLifecycle::Delivered) {
                    qWarning().noquote() << "Cannot activate the manager:" << error;
                }
            });
        }
        if (standalone) {
            preview.setSharedMicrog(standalonePathsValue.sharedMicrog);
            preview.setStandaloneSettings(standalonePathsValue.locale, standalonePathsValue.theme,
                                          standalonePathsValue.location);
            preview.setDeniedPermissions(standalonePathsValue.deniedPermissions);
        }
        qmlRegisterType<PreviewSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
        QScopedPointer<QQuickView> view(Aurora::Application::createView());
        if (standalone || storePreview) {
            view->setTitle(storePreview ? QStringLiteral("Aurora Store") : standalonePathsValue.displayName);
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
        if (standalone || storePreview) {
            StandaloneApkExport *exporter = new StandaloneApkExport(data, sharedLogRoot, picker->host(), nullptr,
                QDir(storePreview ? data : Aurora::Application::filesDir(false).path())
                    .filePath(QStringLiteral(".manager-library-cache")));
            if (!exporter->error().isEmpty()) {
                qWarning("APK handoff unavailable: %s", qPrintable(exporter->error()));
                return 1;
            }
            preview.setSystemHost(exporter->host(), exporter->appsDir());
        }
        if (argc >= 7 && !storePreview) {
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
        view->rootContext()->setContextProperty(QStringLiteral("standaloneFileDiagnostics"),
                                                standalone && standalonePathsValue.fileDiagnostics);
        view->rootContext()->setContextProperty(QStringLiteral("diagnosticLog"), &diagnosticLog);
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

    const QStringList arguments = ManagerLifecycle::launchFileArguments(app->arguments().mid(1));
    SingleInstanceLock instanceLock;
    if (instanceLock.adoptInherited()) {
        qInfo().noquote() << "Launcher single-instance lock adopted:" << instanceLock.path();
        if (backgroundManager) instanceLock.setHeld(false);
    }
    if (!backgroundManager) {
        QString error;
        const ManagerLifecycle::ForwardResult forwarded = ManagerLifecycle::forwardToRunningManager(arguments, &error);
        if (forwarded == ManagerLifecycle::Delivered) return 0;
        if (forwarded == ManagerLifecycle::Failed) {
            qWarning().noquote() << "Cannot activate the running manager:" << error;
            return 1;
        }
        if (forwarded == ManagerLifecycle::Unavailable)
            qWarning().noquote() << "Cannot check for an existing manager:" << error;
    }

    qmlRegisterType<SideMouseButtons>("AndroidCompatibilityLayer.Navigation", 1, 0, "SideMouseButtons");
    qmlRegisterType<PreviewSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
    app->setQuitOnLastWindowClosed(false);
    QPointer<ManagerLifecycle> activeLifecycle;
    QScopedPointer<Probe> probe;
    QScopedPointer<EmbeddedPreview> embeddedPreview;
    QScopedPointer<QQuickView> view;
    QObject::connect(app.data(), &QGuiApplication::lastWindowClosed, app.data(), [&]() {
        if (embeddedPreview) embeddedPreview->stop();
    });
    std::function<bool()> showManager;
    std::function<void()> configureSharedMicrog;
    const auto ensureProbe = [&]() {
        if (probe) return;
        probe.reset(new Probe(Aurora::Application::organizationPathTo(SharedRuntime::location()).toLocalFile(),
                              nullptr, &diagnosticLog));
        probe->setReportWebViewInitializer(initReportWebView);
        QObject::connect(probe.data(), &Probe::runtimeSettingsChanged, probe.data(), [&]() {
            if (activeLifecycle) {
                activeLifecycle->refreshSharedMicrogProvider();
                if (configureSharedMicrog) configureSharedMicrog();
            }
        });
        QObject::connect(probe.data(), &Probe::standaloneRpmError, probe.data(),
                         [&](const QString &, const QString &message) {
            if (view && view->isVisible()) return;
            if (showManager() && probe) probe->externalInstallError(message);
            else qWarning("Cannot display the RPM installation error: %s", qPrintable(message));
        });
    };
    showManager = [&]() -> bool {
        if (view) {
            view->show();
            view->raise();
            view->requestActivate();
            instanceLock.setHeld(true);
            return true;
        }
        QString snapshotError;
        if (!ErrorLog::setStandaloneSnapshotRoot(sharedLogRoot, &snapshotError))
            qWarning().noquote() << "Standalone log snapshots unavailable:" << snapshotError;
        ensureProbe();
        embeddedPreview.reset(new EmbeddedPreview);
        probe->setEmbeddedMicrog(embeddedPreview.data());
        view.reset(Aurora::Application::createView());
        PreviewKeyboard *keyboard = new PreviewKeyboard(view->contentItem());
        embeddedPreview->setRemoteKeyboard(keyboard);
        QObject::connect(embeddedPreview.data(), &EmbeddedPreview::channelError,
                         [](const QString &error) { qWarning().noquote() << "Embedded preview:" << error; });
        probe->setConfirmationWindow(view.data());
        view->rootContext()->setContextProperty(QStringLiteral("probe"), probe.data());
        view->rootContext()->setContextProperty(QStringLiteral("microgPreview"), embeddedPreview.data());
        view->setSource(Aurora::Application::pathTo(QStringLiteral("qml/AndroidCompatibilityLayer.qml")));
        if (view->status() == QQuickView::Error) {
            qWarning("Cannot load the manager window.");
            view.reset();
            probe->setEmbeddedMicrog(nullptr);
            embeddedPreview.reset();
            return false;
        }
        embeddedPreview->setRemoteTheme(view->rootObject()->property("systemDark").toBool(),
                                       view->rootObject()->property("systemAccent").value<QColor>());
        // Hold the launcher lock only while a window exists to raise; without one the next launch
        // must start a forwarding process instead.
        QObject::connect(view.data(), &QWindow::visibleChanged, &instanceLock, &SingleInstanceLock::setHeld);
        view->show();
        instanceLock.setHeld(true);
        return true;
    };
    ManagerLifecycle lifecycle(showManager);
    activeLifecycle = &lifecycle;
    lifecycle.setKeepAlive(true);
    QString configuredWorkerProgram;
    QStringList configuredWorkerArguments;
    QString configuredIdentityDirectory;
    bool sharedMicrogConfigured = false;
    configureSharedMicrog = [&]() {
        QSettings settings;
        if (settings.value(QStringLiteral("googleServices/provider"),
                           QStringLiteral("none")).toString() != QLatin1String("microg"))
            return;

        QString program, identityDirectory, error;
        QStringList arguments;
        if (!prepareSharedMicrogWorker(&program, &arguments, &identityDirectory, &error)) {
            if (sharedMicrogConfigured) {
                lifecycle.disableSharedMicrogWorker();
                sharedMicrogConfigured = false;
                configuredWorkerProgram.clear();
                configuredWorkerArguments.clear();
                configuredIdentityDirectory.clear();
            }
            qWarning().noquote() << "Shared microG backend unavailable:" << error;
            return;
        }
        if (sharedMicrogConfigured && configuredWorkerProgram == program &&
            configuredWorkerArguments == arguments &&
            configuredIdentityDirectory == identityDirectory) return;
        if (sharedMicrogConfigured) {
            lifecycle.disableSharedMicrogWorker();
            sharedMicrogConfigured = false;
        }
        if (!lifecycle.configureSharedMicrogWorker(
                program, arguments, ManagerLifecycle::registryPackageResolver(identityDirectory), &error)) {
            lifecycle.disableSharedMicrogWorker();
            qWarning().noquote() << "Shared microG backend unavailable:" << error;
            return;
        }
        configuredWorkerProgram = program;
        configuredWorkerArguments = arguments;
        configuredIdentityDirectory = identityDirectory;
        sharedMicrogConfigured = true;
    };
    configureSharedMicrog();
    QObject::connect(&lifecycle, &ManagerLifecycle::standaloneLaunchFailed, &lifecycle,
                     [&](const QString &package, const QString &message) {
        if (showManager() && probe) probe->offerStandaloneInstall(package, message);
        else qWarning("Cannot show the app installation offer: %s", qPrintable(message));
    });
    lifecycle.setLaunchHandler([&](const QString &package, const std::function<void(const QString &)> &reply) {
        const auto fail = [&](const QString &error) {
            emit lifecycle.standaloneLaunchFailed(package, error);
            reply(QStringLiteral("error:") + error);
        };
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString appDir = QDir(root).filePath(QStringLiteral("apps/") + package);
        const QString apkPath = QDir(appDir).filePath(QStringLiteral("base.apk"));
        const QString rpmDir = QDir(appDir).filePath(QStringLiteral("standalone-rpm"));
        mg_apk_info info = {};
        if (root.isEmpty() || QFileInfo(appDir).isSymLink() || !QFileInfo(appDir).isDir() ||
            QFileInfo(apkPath).isSymLink() || !QFileInfo(apkPath).isFile() ||
            !mg_apk_read_info(QFile::encodeName(apkPath).constData(), &info) ||
            QString::fromUtf8(info.package) != package) {
            fail(QObject::tr("The app was not found in the manager library."));
            return;
        }
        const QString appId = StandaloneAppMapping::read(rpmDir);
        if (appId.isEmpty()) {
            fail(QObject::tr("No standalone app was created for this library entry."));
            return;
        }
        auto *start = new StandaloneStart(&lifecycle);
        QObject::connect(start, &StandaloneStart::finished, &lifecycle,
                         [&, start, package, reply](bool accepted, const QString &error) {
            if (accepted) reply(QStringLiteral("ok"));
            else {
                emit lifecycle.standaloneLaunchFailed(package, error);
                reply(QStringLiteral("error:") + error);
            }
            start->deleteLater();
        });
        QString error;
        if (!start->start(appId, &error)) {
            start->deleteLater();
            fail(error);
        }
    });
    QObject::connect(&lifecycle, &ManagerLifecycle::fileRequested, [&probe, &lifecycle](const QString &path) {
        QTimer::singleShot(0, &lifecycle, [&probe, path]() {
            if (probe) probe->requestExternalInstall(path);
        });
    });
    QObject::connect(&lifecycle, &ManagerLifecycle::fileRejected, [&probe, &view](const QString &message) {
        if (probe && view && view->isVisible()) probe->externalInstallError(message);
        else qWarning("Rejected external APK request: %s", qPrintable(message));
    });
    QString receiverError;
    const bool receiverStarted = lifecycle.start(backgroundManager, &receiverError);
    if (!receiverStarted) {
        if (backgroundManager) {
            qWarning().noquote() << "Cannot start the background manager:" << receiverError;
            return 1;
        }
        QString forwardError;
        const ManagerLifecycle::ForwardResult forwarded = ManagerLifecycle::forwardToRunningManager(arguments, &forwardError);
        if (forwarded == ManagerLifecycle::Delivered) return 0;
        if (forwarded == ManagerLifecycle::Failed) {
            qWarning().noquote() << "Cannot activate the running manager:" << forwardError;
            return 1;
        }
        qWarning().noquote() << "File-open service unavailable:" << receiverError;
    }
    QTimer handoffTimer;
    QTimer libraryTimer;
    LibraryAppIndex *libraryWorker = nullptr;
    QString claimedHandoff;
    bool handoffFinishing = false;
    QMetaObject::Connection handoffConnection;
    if (receiverStarted) {
        libraryTimer.setInterval(5000);
        QObject::connect(&libraryTimer, &QTimer::timeout, &lifecycle, [&]() {
            if (libraryWorker) return;
            const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            libraryWorker = new LibraryAppIndex(dataRoot.isEmpty() ? QString() : QDir(dataRoot).filePath(QStringLiteral("apps")));
            LibraryAppIndex *worker = libraryWorker;
            QObject::connect(worker, &QThread::finished, &lifecycle, [&lifecycle, &libraryWorker, worker]() {
                libraryWorker = nullptr;
                if (!worker->error.isEmpty()) qWarning().noquote() << "Cannot update the app library list:" << worker->error;
                else lifecycle.setLibraryApps(worker->contents);
                worker->deleteLater();
            });
            worker->start();
        });
        QObject::connect(app.data(), &QCoreApplication::aboutToQuit, &lifecycle, [&]() {
            if (libraryWorker) {
                libraryWorker->wait();
                delete libraryWorker;
                libraryWorker = nullptr;
            }
        });
        libraryTimer.start();
        QTimer::singleShot(0, &libraryTimer, SLOT(timeout()));
        const QString sharedRoot = sharedLogRoot;
        handoffTimer.setInterval(5000);
        QObject::connect(&handoffTimer, &QTimer::timeout, &lifecycle, [&, sharedRoot]() {
            if (handoffFinishing) {
                QString error;
                if (!StandaloneApkExport::removeClaimed(sharedRoot, claimedHandoff, &error)) {
                    qWarning().noquote() << "Cannot remove shared APK request:" << error;
                    return;
                }
                claimedHandoff.clear();
                handoffFinishing = false;
            }
            if (!claimedHandoff.isEmpty() || (probe && (probe->externalInstallBusy() || probe->officialStoreBusy())))
                return;
            QString error;
            const QString path = StandaloneApkExport::takeNext(sharedRoot, &error);
            if (!error.isEmpty()) qWarning().noquote() << "Cannot inspect shared APK requests:" << error;
            if (path.isEmpty()) return;
            ensureProbe();
            if (!probe->autoInstallRpm() && !showManager()) {
                qWarning("Cannot show the manager for the shared APK request.");
                return;
            }
            claimedHandoff = path;
            handoffConnection = QObject::connect(probe.data(), &Probe::storeHandoffFinished, &lifecycle,
                                                  [&](bool retry, bool installed, const QByteArray &fingerprint) {
                QObject::disconnect(handoffConnection);
                if (retry) {
                    QTimer::singleShot(30000, &lifecycle, [&]() { claimedHandoff.clear(); });
                } else {
                    if (!lifecycle.recordStoreResult(claimedHandoff, installed, fingerprint))
                        qWarning("Cannot record the shared APK import result.");
                    else
                        qInfo("APK handoff: manager recorded import %s.", installed ? "success" : "failure");
                    handoffFinishing = true;
                    if (!installed && (!view || !view->isVisible())) {
                        QTimer::singleShot(0, &lifecycle, [&]() {
                            const QString message = probe->result();
                            if (showManager() && probe) probe->externalInstallError(message);
                            else qWarning("Cannot display the APK import error: %s", qPrintable(message));
                        });
                    }
                }
            });
            if (!probe->requestStoreHandoff(path, &error)) {
                QObject::disconnect(handoffConnection);
                claimedHandoff.clear();
                qWarning().noquote() << "Cannot start shared APK import:" << error;
            }
        });
        handoffTimer.start();
        QTimer::singleShot(0, &handoffTimer, SLOT(timeout()));
    }
    QString collectorError;
    if (!ErrorLog::startCollector(&collectorError)) {
        qWarning().noquote() << "Shared in-memory log unavailable:" << collectorError;
    } else {
        QString storageError;
        if (!ErrorLog::startPersistence(Aurora::Application::filesDir(false).path(), &storageError))
            qWarning().noquote() << "Private log storage unavailable:" << storageError;
    }
    if (!backgroundManager) {
        if (arguments.isEmpty()) {
            if (!showManager()) return 1;
            if (!receiverError.isEmpty()) probe->externalInstallError(receiverError);
        } else {
            if (!lifecycle.submit(arguments)) return 1;
            if (!view) return 1;
            if (!receiverError.isEmpty()) probe->externalInstallError(receiverError);
        }
    }
    return app->exec();
}
