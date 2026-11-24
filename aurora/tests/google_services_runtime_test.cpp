#include "../google_services_runtime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

struct FakeApk {
    std::string package;
    bool microG;
    bool supported;
    bool split;
};

static std::map<std::string, FakeApk> apks;

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    const auto found = apks.find(path ? path : "");
    if (found == apks.end()) {
        std::strcpy(info->error, "Not a valid test APK");
        return false;
    }
    std::strcpy(info->package, found->second.package.c_str());
    info->supported = found->second.supported;
    info->split_required = found->second.split;
    return true;
}

extern "C" bool mg_apk_gms_name(const char *package)
{
    static const char suffix[] = ".android.gms";
    const size_t size = package ? std::strlen(package) : 0;
    const size_t suffixSize = sizeof(suffix) - 1;
    return size > suffixSize && std::strcmp(package + size - suffixSize, suffix) == 0;
}

extern "C" bool mg_apk_is_microg(const char *path)
{
    const auto found = apks.find(path ? path : "");
    return found != apks.end() && found->second.microG;
}

extern "C" void mg_apk_info_free(mg_apk_info *) {}

static bool writeFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

static void setProvider(const QString &provider, const QString &gms)
{
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/provider"), provider);
    settings.setValue(QStringLiteral("googleServices/gmsApkPath"), gms);
    settings.sync();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GoogleServicesRuntime"));
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());

    const QDir temp(directory.path());
    const QString microG = temp.filePath(QStringLiteral("microg.apk"));
    const QString gms = temp.filePath(QStringLiteral("gms.apk"));
    if (!writeFile(microG) || !writeFile(gms)) return 1;
    apks[microG.toStdString()] = {"com.google.android.gms", true, false, false};
    apks[temp.filePath(QStringLiteral("revanced.apk")).toStdString()] =
        {"app.revanced.android.gms", true, true, false};
    apks[gms.toStdString()] = {"com.google.android.gms", false, true, false};

    megler_run_config run = {};
    GoogleServicesRuntime runtime(directory.path());
    QString error;

    setProvider(QStringLiteral("none"), QString());
    if (!runtime.apply(run, &error) || run.gms_apk_path || run.gms_splits_dir || run.gsf_apk_path || run.gms_package)
        return 1;

    setProvider(QStringLiteral("microg"), QString());
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path) return 1;

    if (!temp.mkpath(QStringLiteral("services"))) return 1;
    const QString bundled = temp.filePath(QStringLiteral("services/GmsCore.apk"));
    if (!writeFile(bundled)) return 1;
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path) return 1;
    apks[bundled.toStdString()] = {"com.google.android.gms", true, true, true};
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path) return 1;
    apks[bundled.toStdString()].split = false;
    if (!runtime.apply(run, &error) || !run.gms_apk_path || bundled.toUtf8() != run.gms_apk_path ||
        run.gms_package || !error.isEmpty())
        return 1;

    setProvider(QStringLiteral("microg"), temp.filePath(QStringLiteral("missing.apk")));
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path) return 1;

    setProvider(QStringLiteral("microg"), microG);
    if (!runtime.apply(run, &error) || !run.gms_apk_path || microG.toUtf8() != run.gms_apk_path ||
        run.gsf_apk_path || run.gms_package)
        return 1;

    const QString revanced = temp.filePath(QStringLiteral("revanced.apk"));
    if (!writeFile(revanced)) return 1;
    setProvider(QStringLiteral("microg"), revanced);
    if (!runtime.apply(run, &error) || !run.gms_package ||
        revanced.toUtf8() != run.gms_apk_path ||
        QByteArray("app.revanced.android.gms") != run.gms_package)
        return 1;

    setProvider(QStringLiteral("microg"), gms);
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path) return 1;

    setProvider(QStringLiteral("google"), gms);
    if (runtime.apply(run, &error) || error.isEmpty() || run.gms_apk_path || run.gsf_apk_path)
        return 1;

    if (!runtime.applySharedModule(run, microG, &error) || !run.gms_apk_path ||
        microG.toUtf8() != run.gms_apk_path || run.gms_package) return 1;
    if (runtime.applySharedModule(run, gms, &error) || error.isEmpty() || run.gms_apk_path) return 1;
    if (runtime.applySharedModule(run, revanced, &error) || error.isEmpty() || run.gms_apk_path) return 1;
    apks[microG.toStdString()].split = true;
    if (runtime.applySharedModule(run, microG, &error) || error.isEmpty() || run.gms_apk_path) return 1;
    if (runtime.applySharedModule(run, temp.filePath(QStringLiteral("missing.apk")), &error) ||
        error.isEmpty() || run.gms_apk_path) return 1;

    std::puts("Google services runtime settings OK");
    return 0;
}
