#include "../rpm_standalone.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLockFile>
#include <QSettings>

#include "../apk_store.h"
#include "../permission_settings.h"
#include "../standalone_app_mapping.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

static QByteArray apkReport = R"({"ok":true,"package":"org.example.app","permissions":["android.permission.INTERNET","android.permission.CAMERA","android.permission.ACCESS_FINE_LOCATION","android.permission.BLUETOOTH_CONNECT"],"permissionMaxSdk":[0,0,0,0]})";
extern "C" char *megler_apk_report(const char *) {
    char *result = static_cast<char *>(std::malloc(size_t(apkReport.size()) + 1));
    if (result) std::memcpy(result, apkReport.constData(), size_t(apkReport.size()) + 1);
    return result;
}
extern "C" char *megler_apk_report_splits(const char *path, const char *) { return megler_apk_report(path); }
extern "C" bool megler_apk_icon(const char *, uint8_t **, size_t *) { return false; }
extern "C" bool megler_apk_icon_splits(const char *, const char *, uint8_t **, size_t *) { return false; }
extern "C" void megler_free(void *ptr) { std::free(ptr); }

static bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const QDir root(QString::fromLocal8Bit(argv[1]));
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("StandaloneRpm"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
    QSettings settings;
    settings.setValue(QStringLiteral("runtime/locale"), QStringLiteral("de-DE"));
    settings.setValue(QStringLiteral("runtime/theme"), QStringLiteral("dark"));
    settings.sync();
    if (settings.status() != QSettings::NoError) return 1;
    const QString store = root.filePath(QStringLiteral("store"));
    const QString runtime = root.filePath(QStringLiteral("runtime"));
    const QString qml = root.filePath(QStringLiteral("qml"));
    const QString appDir = QDir(store).filePath(QStringLiteral("org.example.app"));
    for (const QString &path : {appDir + QStringLiteral("/splits"), appDir + QStringLiteral("/data"), runtime, qml})
        if (!QDir().mkpath(path)) return 1;
    if (!write(root.filePath(QStringLiteral("source.apk")), "apk content") ||
        !QFile::link(root.filePath(QStringLiteral("source.apk")), appDir + QStringLiteral("/base.apk")) ||
        !write(appDir + QStringLiteral("/splits/config.apk"), "split content") ||
        !write(appDir + QStringLiteral("/data/secret"), "private data must stay private") ||
        !write(runtime + QStringLiteral("/runtime.dex"), "runtime content") ||
        !write(runtime + QStringLiteral("/.standalone-abi"), QByteArray(STANDALONE_RUNTIME_ABI) + "\n") ||
        !write(qml + QStringLiteral("/androidcompatibilitylayer_ru.qm"), "translation catalog content") ||
        !write(root.filePath(QStringLiteral("qml-source.qml")), "linked qml content") ||
        !write(root.filePath(QStringLiteral("binary-source")), "binary content") ||
        !QFile::link(root.filePath(QStringLiteral("binary-source")), root.filePath(QStringLiteral("binary")))) return 1;
    if (!QDir().mkpath(runtime + QStringLiteral("/etc")) ||
        !write(root.filePath(QStringLiteral("font-source.xml")), "linked runtime font data") ||
        !QFile::link(root.filePath(QStringLiteral("font-source.xml")),
                     runtime + QStringLiteral("/etc/fonts.xml")) ||
        !QFile::link(root.filePath(QStringLiteral("qml-source.qml")),
                     qml + QStringLiteral("/AndroidFilePicker.qml")) ||
        !QFile::link(root.filePath(QStringLiteral("font-source.xml")),
                     appDir + QStringLiteral("/splits/linked.apk")) ||
        !QFile::link(runtime + QStringLiteral("/etc"), runtime + QStringLiteral("/linked-dir")) ||
        !QFile::link(runtime + QStringLiteral("/etc"), qml + QStringLiteral("/linked-dir")) ||
        !QFile::link(runtime, root.filePath(QStringLiteral("runtime-link"))) ||
        !QFile::link(qml, root.filePath(QStringLiteral("qml-link")))) return 1;
    QImage icon(120, 60, QImage::Format_ARGB32);
    icon.fill(Qt::red);
    QString output, error;
    QStringList denied;
    if (!PermissionSettings::read(appDir, &denied, &error) || !denied.isEmpty() ||
        !PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera") << QStringLiteral("location"), &error) ||
        !PermissionSettings::read(appDir, &denied, &error) ||
        denied != (QStringList() << QStringLiteral("camera") << QStringLiteral("location"))) return 1;
    QVariantList requested = PermissionSettings::requested(
        QVariantList() << QStringLiteral("android.permission.CAMERA")
                       << QStringLiteral("android.permission.ACCESS_FINE_LOCATION")
                       << QStringLiteral("android.permission.READ_CONTACTS"),
        QVariantList() << 0 << 0 << 32, denied);
    if (requested.size() != 2 || requested.at(0).toMap().value(QStringLiteral("allowed")).toBool() ||
        requested.at(1).toMap().value(QStringLiteral("allowed")).toBool()) return 1;
    const QString out = root.filePath(QStringLiteral("out"));
    const QString binary = root.filePath(QStringLiteral("binary"));
    const QString runtimeLink = root.filePath(QStringLiteral("runtime-link"));
    const QString qmlLink = root.filePath(QStringLiteral("qml-link"));
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) || output.isEmpty()) return 1;
    const QString cacheDir = appDir + QStringLiteral("/standalone-rpm");
    if (!QDir().mkpath(cacheDir)) return 1;
    const QString cached = QDir(cacheDir).filePath(QFileInfo(output).fileName());
    if (!QFile::copy(output, cached) || !StandaloneAppMapping::save(cacheDir, cached)) return 1;
    const auto fingerprint = [&]() {
        return RpmStandalone::cacheFingerprint(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
                                                store, runtimeLink, qmlLink, binary, false);
    };
    const QByteArray original = fingerprint();
    if (original.isEmpty() || !RpmStandalone::saveCache(cacheDir, cached, original) ||
        RpmStandalone::cachedRpm(cacheDir, fingerprint()) != cached) return 1;
    int generated = 0;
    QString resolved;
    bool hit = false;
    const auto generate = [&](QString *path) {
        ++generated;
        *path = QStringLiteral("generated.rpm");
        return true;
    };
    const auto resolve = [&]() {
        return RpmStandalone::reuseOrGenerate([&]() { return RpmStandalone::cachedRpm(cacheDir, fingerprint()); },
                                               generate, &resolved, &hit);
    };
    if (!resolve() || !hit || generated != 0 || resolved != cached) return 1;
    if (!PermissionSettings::save(appDir, QStringList() << QStringLiteral("location") << QStringLiteral("camera"), &error) ||
        fingerprint() != original ||
        !PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera"), &error) ||
        !RpmStandalone::cachedRpm(cacheDir, fingerprint()).isEmpty() ||
        !PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera") << QStringLiteral("location")
                                                         << QStringLiteral("microphone"), &error) ||
        !RpmStandalone::cachedRpm(cacheDir, fingerprint()).isEmpty() ||
        !PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera") << QStringLiteral("location"), &error))
        return 1;
    if (!PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera"), &error)) return 1;
    const QByteArray narrower = fingerprint();
    if (!RpmStandalone::saveCache(cacheDir, cached, narrower) ||
        !PermissionSettings::save(appDir, QStringList() << QStringLiteral("camera") << QStringLiteral("location"), &error) ||
        !RpmStandalone::cachedRpm(cacheDir, fingerprint()).isEmpty() ||
        !RpmStandalone::saveCache(cacheDir, cached, original)) return 1;
    if (!write(root.filePath(QStringLiteral("source.apk")), "apk contenT") ||
        RpmStandalone::cachedRpm(cacheDir, fingerprint()) == cached ||
        !resolve() || hit || generated != 1 || resolved != QLatin1String("generated.rpm") ||
        !write(root.filePath(QStringLiteral("source.apk")), "apk content") ||
        !write(root.filePath(QStringLiteral("binary-source")), "binary contenT") ||
        RpmStandalone::cachedRpm(cacheDir, fingerprint()) == cached ||
        !write(root.filePath(QStringLiteral("binary-source")), "binary content") ||
        !write(appDir + QStringLiteral("/splits/config.apk"), "split contenT") ||
        RpmStandalone::cachedRpm(cacheDir, fingerprint()) == cached ||
        !write(appDir + QStringLiteral("/splits/config.apk"), "split content") ||
        !write(root.filePath(QStringLiteral("qml-source.qml")), "changed qml content") ||
        !RpmStandalone::cachedRpm(cacheDir, fingerprint()).isEmpty() ||
        !write(root.filePath(QStringLiteral("qml-source.qml")), "linked qml content") ||
        !write(runtime + QStringLiteral("/.standalone-abi"), "999999\n") ||
        !fingerprint().isEmpty() ||
        !write(runtime + QStringLiteral("/.standalone-abi"), QByteArray(STANDALONE_RUNTIME_ABI) + "\n") ||
        RpmStandalone::cachedRpm(cacheDir, fingerprint()) != cached ||
        !write(cached, "corrupt") || !RpmStandalone::cachedRpm(cacheDir, fingerprint()).isEmpty()) return 1;
    QFile rpm(output);
    if (!rpm.open(QIODevice::ReadOnly)) return 1;
    const QByteArray contents = rpm.readAll();
    if (contents.contains("private data must stay private") ||
        contents.contains("runtime content") ||
        contents.contains("/android-runtime/") ||
        !contents.contains(QByteArray("\"sharedRuntimeAbi\":\"") + STANDALONE_RUNTIME_ABI + "\"") ||
        !contents.contains("translation catalog content") ||
        !contents.contains("\"standaloneSnapshots\":true") ||
        !contents.contains("\"fileDiagnostics\":false") ||
        contents.contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager;Documents\n") ||
        !contents.contains("OrganizationName=ru.androidcompatibilitylayer\nApplicationName=Android") ||
        !contents.contains("\"deniedPermissions\":[\"camera\",\"location\"]") ||
        !contents.contains("Permissions=Internet;Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager;Bluetooth\n")) return 1;
    const QString first = output;
    apkReport = R"({"ok":true,"package":"org.example.app","permissions":["android.permission.CAMERA","android.permission.ACCESS_FINE_LOCATION","android.permission.RECORD_AUDIO","android.permission.BODY_SENSORS","android.permission.READ_MEDIA_IMAGES","android.permission.READ_MEDIA_AUDIO","android.permission.CALL_PHONE","android.permission.INTERNET"],"permissionMaxSdk":[0,0,0,0,0,0,0,28]})";
    if (!PermissionSettings::save(appDir, QStringList(), &error) ||
        !RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error)) return 1;
    QFile selectedRpm(output);
    if (!selectedRpm.open(QIODevice::ReadOnly) ||
        !selectedRpm.readAll().contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager;Camera;Location;Microphone;Call;Sensors;Pictures;Music\n")) return 1;
    apkReport = R"({"ok":true,"package":"org.example.app","permissions":[],"permissionMaxSdk":[]})";
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error)) return 1;
    QFile minimalRpm(output);
    if (!minimalRpm.open(QIODevice::ReadOnly) ||
        !minimalRpm.readAll().contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager\n")) return 1;
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error, true)) return 1;
    QFile diagnosticRpm(output);
    if (!diagnosticRpm.open(QIODevice::ReadOnly)) return 1;
    const QByteArray diagnosticContents = diagnosticRpm.readAll();
    if (!diagnosticContents.contains("\"fileDiagnostics\":true") ||
        !diagnosticContents.contains("\"standaloneSnapshots\":true") ||
        diagnosticContents.contains("Documents\n")) return 1;
    apkReport = R"({"ok":true,"package":"org.example.app","permissions":["android.permission.NEARBY_WIFI_DEVICES","android.permission.UWB_RANGING"],"permissionMaxSdk":[0,0]})";
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error)) return 1;
    QFile nearbyRpm(output);
    if (!nearbyRpm.open(QIODevice::ReadOnly) ||
        !nearbyRpm.readAll().contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager\n")) return 1;
    apkReport = R"({"ok":true,"package":"org.example.app","permissions":["android.permission.READ_EXTERNAL_STORAGE","android.permission.READ_MEDIA_IMAGES","android.permission.READ_MEDIA_VIDEO","android.permission.READ_MEDIA_AUDIO"],"permissionMaxSdk":[0,0,0,0]})";
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error)) return 1;
    QFile storageRpm(output);
    if (!storageRpm.open(QIODevice::ReadOnly)) return 1;
    const QByteArray storageContents = storageRpm.readAll();
    if (!storageContents.contains("\"standaloneSnapshots\":true") ||
        !storageContents.contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager;UserDirs\n")) return 1;
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error, true)) return 1;
    QFile diagnosticStorageRpm(output);
    if (!diagnosticStorageRpm.open(QIODevice::ReadOnly) ||
        !diagnosticStorageRpm.readAll().contains("Permissions=Audio;ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager;ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager;UserDirs\n")) return 1;
    apkReport = R"({"ok":true,"package":"org.example.app"})";
    if (RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) || error.isEmpty()) return 1;
    apkReport = R"({"ok":true,"package":"org.example.app","permissions":[],"permissionMaxSdk":[]})";
    if (!RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) || output == first) return 1;
    const QString cycle = qml + QStringLiteral("/cycle");
    if (!QFile::link(qml, cycle) ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) ||
        !error.contains(QStringLiteral("cycle")) || !QFile::remove(cycle)) return 1;
    const QString dataLink = qml + QStringLiteral("/private");
    if (!QFile::link(appDir + QStringLiteral("/data"), dataLink) ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) ||
        !error.contains(QStringLiteral("private app data")) || !QFile::remove(dataLink)) return 1;
    if (!QFile::remove(runtime + QStringLiteral("/.standalone-abi")) ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) ||
        error.isEmpty()) return 1;
    if (!write(runtime + QStringLiteral("/.standalone-abi"),
               QByteArray(STANDALONE_RUNTIME_ABI) == "1" ? "2\n" : "1\n") ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) ||
        !error.contains(QStringLiteral("incompatible"))) return 1;
#ifdef Q_OS_UNIX
    const QString broken = qml + QStringLiteral("/broken");
    if (::symlink("missing-target", QFile::encodeName(broken).constData()) != 0 ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtimeLink, qmlLink, binary, out, &output, &error) ||
        error.isEmpty() || !QFile::remove(broken)) return 1;
#endif
    ApkStore appStore(store);
    QLockFile lock(appStore.lockPath(QStringLiteral("org.example.app")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0) ||
        RpmStandalone::createUnsigned(QStringLiteral("org.example.app"), QStringLiteral("Example"), icon,
            store, runtime, qml, binary, out, &output, &error) || error.isEmpty()) return 1;
    return 0;
}
