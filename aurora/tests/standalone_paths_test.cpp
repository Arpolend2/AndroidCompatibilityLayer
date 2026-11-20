#include "../standalone_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdlib>

extern "C" char *megler_apk_report(const char *) { return nullptr; }
extern "C" char *megler_apk_report_splits(const char *, const char *) { return nullptr; }
extern "C" bool megler_apk_icon(const char *, uint8_t **, size_t *) { return false; }
extern "C" bool megler_apk_icon_splits(const char *, const char *, uint8_t **, size_t *) { return false; }
extern "C" void megler_free(void *ptr) { std::free(ptr); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    const QString root = QDir(dir.path()).filePath(QStringLiteral("resources"));
    const QString storage = QDir(dir.path()).filePath(QStringLiteral("private"));
    if (!QDir().mkpath(QDir(root).filePath(QStringLiteral("apk"))) ||
        !QDir().mkpath(QDir(root).filePath(QStringLiteral("android-runtime")))) return 1;
    QFile apk(QDir(root).filePath(QStringLiteral("apk/base.apk")));
    if (!apk.open(QIODevice::WriteOnly) || apk.write("test") != 4) return 1;
    apk.close();
    const QString package = QStringLiteral("org.example.app");
    const QString appId = QStringLiteral("ru.androidcompatibilitylayer.Android") + QString::fromLatin1(
        QCryptographicHash::hash(package.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    const QString metadataPath = QDir(root).filePath(QStringLiteral("standalone.json"));
    auto metadata = [&](const QString &id, const QString &pkg, const QString &locale = QString(),
                        const QString &theme = QString(), const QString &location = QString()) {
        QJsonObject object;
        object.insert(QStringLiteral("appId"), id);
        object.insert(QStringLiteral("package"), pkg);
        object.insert(QStringLiteral("displayName"), QStringLiteral("Example app"));
        object.insert(QStringLiteral("locale"), locale);
        object.insert(QStringLiteral("theme"), theme);
        object.insert(QStringLiteral("location"), location);
        QFile file(metadataPath);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(QJsonDocument(object).toJson()) > 0;
    };
    if (!metadata(appId, package, QStringLiteral("zh-Hant-TW"), QStringLiteral("dark"), QStringLiteral("55.5,37.5"))) return 1;
    StandalonePaths paths;
    QString error;
    if (!standalonePaths(appId, appId, root, storage, &paths, &error) ||
        paths.apk != apk.fileName() || paths.displayName != QStringLiteral("Example app") ||
        paths.locale != QStringLiteral("zh-Hant-TW") || paths.theme != QStringLiteral("dark") ||
        paths.location != QStringLiteral("55.5,37.5") ||
        paths.data != QDir(storage).filePath(QStringLiteral("apps/org.example.app/data")) ||
        paths.fileDiagnostics || QFileInfo::exists(storage)) return 1;
    {
        QFile file(metadataPath);
        if (!file.open(QIODevice::ReadOnly)) return 1;
        QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        object.insert(QStringLiteral("fileDiagnostics"), true);
        object.insert(QStringLiteral("deniedPermissions"), QJsonArray() << QStringLiteral("camera") << QStringLiteral("location"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (!standalonePaths(appId, appId, root, storage, &paths, &error) || !paths.fileDiagnostics ||
            paths.deniedPermissions != (QStringList() << QStringLiteral("camera") << QStringLiteral("location"))) return 1;
        object.insert(QStringLiteral("fileDiagnostics"), QStringLiteral("yes"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
        object.insert(QStringLiteral("fileDiagnostics"), true);
        object.insert(QStringLiteral("deniedPermissions"), QJsonArray() << QStringLiteral("unknown"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    }
    if (!metadata(appId, package, QStringLiteral("bad--tag"), QStringLiteral("dark")) ||
        standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    if (!metadata(appId, package, QString(), QStringLiteral("invalid")) ||
        standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    if (!metadata(appId, package, QString(), QString(), QStringLiteral("91,0")) ||
        standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    if (standalonePaths(appId, QStringLiteral("wrong"), root, storage, &paths, &error) || error.isEmpty()) return 1;
    if (!metadata(QStringLiteral("ru.androidcompatibilitylayer.Android000000000000000000000000"), QStringLiteral("org.example.other")) ||
        standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    if (!metadata(appId, QStringLiteral("../escape")) ||
        standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    return 0;
}
