#include "standalone_paths.h"

#include "apk_store.h"
#include "runtime_settings.h"
#include "permission_settings.h"
#include "shared_runtime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

bool standalonePaths(const QString &appId, const QString &executableName,
                     const QString &resourceRoot, const QString &privateRoot,
                     StandalonePaths *paths, QString *error, const QString &sharedRuntimeRoot,
                     StandaloneRuntimeError *runtimeError)
{
    if (error) error->clear();
    if (runtimeError) *runtimeError = StandaloneRuntimeError::None;
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    static const QRegularExpression identity(QStringLiteral("^ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}$"));
    if (!paths || !identity.match(appId).hasMatch() || executableName != appId ||
        resourceRoot.isEmpty() || privateRoot.isEmpty())
        return fail(QStringLiteral("Invalid standalone application identity or storage."));

    QFile metadata(QDir(resourceRoot).filePath(QStringLiteral("standalone.json")));
    if (!metadata.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot read standalone metadata: %1").arg(metadata.errorString()));
    if (metadata.size() > 4096)
        return fail(QStringLiteral("Standalone metadata is too large."));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(metadata.readAll(), &parseError);
    if (metadata.error() != QFile::NoError || parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("Invalid standalone metadata."));
    const QJsonObject object = doc.object();
    const QString package = object.value(QStringLiteral("package")).toString();
    const QString displayName = object.value(QStringLiteral("displayName")).toString();
    const QJsonValue localeValue = object.value(QStringLiteral("locale"));
    const QJsonValue themeValue = object.value(QStringLiteral("theme"));
    const QJsonValue locationValue = object.value(QStringLiteral("location"));
    const QString locale = localeValue.toString();
    const QString theme = themeValue.toString();
    const QString location = locationValue.toString();
    const QJsonValue permissionsValue = object.value(QStringLiteral("deniedPermissions"));
    const QJsonValue snapshotsValue = object.value(QStringLiteral("standaloneSnapshots"));
    const QJsonValue diagnosticsValue = object.value(QStringLiteral("fileDiagnostics"));
    const QJsonValue sharedMicrogValue = object.value(QStringLiteral("sharedMicrog"));
    QStringList deniedPermissions;
    const ApkStore store(QDir(privateRoot).filePath(QStringLiteral("apps")));
    const QString data = store.dataPath(package);
    const QString lock = store.lockPath(package);
    const QString apk = QDir(resourceRoot).filePath(QStringLiteral("apk/base.apk"));
    const QJsonValue runtimeAbi = object.value(QStringLiteral("sharedRuntimeAbi"));
    if (object.value(QStringLiteral("appId")).toString() != appId || displayName.isEmpty() ||
        displayName.size() > 512 ||
        (!localeValue.isUndefined() && !localeValue.isString()) ||
        (!themeValue.isUndefined() && !themeValue.isString()) ||
        (!locationValue.isUndefined() && !locationValue.isString()) ||
        (!snapshotsValue.isUndefined() && !snapshotsValue.isBool()) ||
        (!diagnosticsValue.isUndefined() && !diagnosticsValue.isBool()) ||
        (!sharedMicrogValue.isUndefined() && !sharedMicrogValue.isBool()) ||
        (!permissionsValue.isUndefined() && !PermissionSettings::parse(permissionsValue, &deniedPermissions)) ||
        (!locale.isEmpty() && !RuntimeSettings::validLocale(locale)) ||
        !RuntimeSettings::validTheme(theme) ||
        (!location.isEmpty() && !RuntimeSettings::parseLocation(location, nullptr, nullptr)) ||
        data.isEmpty() || lock.isEmpty() ||
        (!runtimeAbi.isUndefined() && !SharedRuntime::validVersion(runtimeAbi.toString())) ||
        !QFileInfo(apk).isFile())
        return fail(QStringLiteral("Standalone package contents are missing or invalid."));
    QString androidRoot = QDir(resourceRoot).filePath(QStringLiteral("android-runtime"));
    if (runtimeAbi.isUndefined()) {
        if (!QFileInfo(androidRoot).isDir())
            return fail(QStringLiteral("Standalone package contents are missing or invalid."));
    } else {
        const QString available = SharedRuntime::version(sharedRuntimeRoot);
        if (available.isEmpty()) {
            if (runtimeError) *runtimeError = StandaloneRuntimeError::Unavailable;
            return fail(QStringLiteral("The shared Android runtime is unavailable."));
        }
        if (available != runtimeAbi.toString()) {
            if (runtimeError) *runtimeError = StandaloneRuntimeError::Incompatible;
            return fail(QStringLiteral("The shared Android runtime is incompatible."));
        }
        androidRoot = sharedRuntimeRoot;
    }
    paths->androidRoot = androidRoot;
    paths->apk = apk;
    paths->data = data;
    paths->lock = lock;
    paths->displayName = displayName;
    paths->locale = locale;
    paths->theme = theme;
    paths->location = location;
    paths->deniedPermissions = deniedPermissions;
    paths->standaloneSnapshots = snapshotsValue.toBool(false);
    paths->fileDiagnostics = diagnosticsValue.toBool(false);
    paths->sharedMicrog = sharedMicrogValue.toBool(false);
    return true;
}
