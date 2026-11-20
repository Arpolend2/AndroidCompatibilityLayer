#include "standalone_paths.h"

#include "apk_store.h"
#include "runtime_settings.h"
#include "permission_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

bool standalonePaths(const QString &appId, const QString &executableName,
                     const QString &resourceRoot, const QString &privateRoot,
                     StandalonePaths *paths, QString *error)
{
    if (error) error->clear();
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
    const QJsonValue diagnosticsValue = object.value(QStringLiteral("fileDiagnostics"));
    QStringList deniedPermissions;
    const ApkStore store(QDir(privateRoot).filePath(QStringLiteral("apps")));
    const QString data = store.dataPath(package);
    const QString lock = store.lockPath(package);
    const QString apk = QDir(resourceRoot).filePath(QStringLiteral("apk/base.apk"));
    const QString androidRoot = QDir(resourceRoot).filePath(QStringLiteral("android-runtime"));
    if (object.value(QStringLiteral("appId")).toString() != appId || displayName.isEmpty() ||
        displayName.size() > 512 ||
        (!localeValue.isUndefined() && !localeValue.isString()) ||
        (!themeValue.isUndefined() && !themeValue.isString()) ||
        (!locationValue.isUndefined() && !locationValue.isString()) ||
        (!diagnosticsValue.isUndefined() && !diagnosticsValue.isBool()) ||
        (!permissionsValue.isUndefined() && !PermissionSettings::parse(permissionsValue, &deniedPermissions)) ||
        (!locale.isEmpty() && !RuntimeSettings::validLocale(locale)) ||
        !RuntimeSettings::validTheme(theme) ||
        (!location.isEmpty() && !RuntimeSettings::parseLocation(location, nullptr, nullptr)) ||
        data.isEmpty() || lock.isEmpty() ||
        !QFileInfo(apk).isFile() || !QFileInfo(androidRoot).isDir())
        return fail(QStringLiteral("Standalone package contents are missing or invalid."));
    paths->androidRoot = androidRoot;
    paths->apk = apk;
    paths->data = data;
    paths->lock = lock;
    paths->displayName = displayName;
    paths->locale = locale;
    paths->theme = theme;
    paths->location = location;
    paths->deniedPermissions = deniedPermissions;
    paths->fileDiagnostics = diagnosticsValue.toBool(false);
    return true;
}
