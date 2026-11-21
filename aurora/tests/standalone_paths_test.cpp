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
        paths.androidRoot != QDir(root).filePath(QStringLiteral("android-runtime")) ||
        paths.apk != apk.fileName() || paths.displayName != QStringLiteral("Example app") ||
        paths.locale != QStringLiteral("zh-Hant-TW") || paths.theme != QStringLiteral("dark") ||
        paths.location != QStringLiteral("55.5,37.5") ||
        paths.data != QDir(storage).filePath(QStringLiteral("apps/org.example.app/data")) ||
        paths.standaloneSnapshots || paths.fileDiagnostics || paths.sharedMicrog || QFileInfo::exists(storage)) return 1;
    {
        QFile file(metadataPath);
        if (!file.open(QIODevice::ReadOnly)) return 1;
        QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        object.insert(QStringLiteral("standaloneSnapshots"), true);
        object.insert(QStringLiteral("sharedMicrog"), true);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (!standalonePaths(appId, appId, root, storage, &paths, &error) ||
            !paths.standaloneSnapshots || paths.fileDiagnostics || !paths.sharedMicrog) return 1;
        object.insert(QStringLiteral("sharedMicrog"), QStringLiteral("yes"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
        object.insert(QStringLiteral("sharedMicrog"), true);
        object.insert(QStringLiteral("standaloneSnapshots"), QStringLiteral("yes"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error) || error.isEmpty()) return 1;
    }
    if (!metadata(appId, package, QStringLiteral("zh-Hant-TW"), QStringLiteral("dark"),
                  QStringLiteral("55.5,37.5"))) return 1;
    {
        QFile file(metadataPath);
        if (!file.open(QIODevice::ReadOnly)) return 1;
        QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        object.insert(QStringLiteral("sharedRuntimeAbi"), QStringLiteral("1"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        const QString shared = QDir(dir.path()).filePath(QStringLiteral("shared"));
        StandaloneRuntimeError runtimeError = StandaloneRuntimeError::None;
        if (standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            runtimeError != StandaloneRuntimeError::Unavailable || error.isEmpty()) return 1;
        if (!QDir().mkpath(shared)) return 1;
        QFile marker(QDir(shared).filePath(QStringLiteral(".standalone-abi")));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("1\n") != 2) return 1;
        marker.close();
        if (!standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            runtimeError != StandaloneRuntimeError::None || paths.androidRoot != shared ||
            paths.apk != apk.fileName()) return 1;
        const QString firstData = paths.data;
        const QString providerDir = QDir(shared).filePath(QStringLiteral("app/webview"));
        if (!QDir().mkpath(providerDir)) return 1;
        QFile provider(QDir(providerDir).filePath(QStringLiteral("webview.apk")));
        if (!provider.open(QIODevice::WriteOnly) || provider.write("provider") != 8) return 1;
        provider.close();
        if (!standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            paths.androidRoot != shared || paths.apk != apk.fileName() || paths.data != firstData ||
            !QFileInfo::exists(QDir(paths.androidRoot).filePath(QStringLiteral("app/webview/webview.apk")))) return 1;
        const QString otherPackage = QStringLiteral("org.example.other");
        const QString otherId = QStringLiteral("ru.androidcompatibilitylayer.Android") + QString::fromLatin1(
            QCryptographicHash::hash(otherPackage.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        const QString otherRoot = QDir(dir.path()).filePath(QStringLiteral("other-resources"));
        const QString otherStorage = QDir(dir.path()).filePath(QStringLiteral("other-private"));
        if (!QDir().mkpath(QDir(otherRoot).filePath(QStringLiteral("apk")))) return 1;
        QFile otherApk(QDir(otherRoot).filePath(QStringLiteral("apk/base.apk")));
        if (!otherApk.open(QIODevice::WriteOnly) || otherApk.write("other") != 5) return 1;
        otherApk.close();
        QJsonObject otherObject = object;
        otherObject.insert(QStringLiteral("appId"), otherId);
        otherObject.insert(QStringLiteral("package"), otherPackage);
        QFile otherMetadata(QDir(otherRoot).filePath(QStringLiteral("standalone.json")));
        if (!otherMetadata.open(QIODevice::WriteOnly) ||
            otherMetadata.write(QJsonDocument(otherObject).toJson()) < 0) return 1;
        otherMetadata.close();
        if (!standalonePaths(otherId, otherId, otherRoot, otherStorage, &paths, &error, shared, &runtimeError) ||
            paths.androidRoot != shared || paths.apk != otherApk.fileName() ||
            paths.data != QDir(otherStorage).filePath(QStringLiteral("apps/org.example.other/data")) ||
            paths.data == firstData) return 1;
        otherObject.remove(QStringLiteral("sharedRuntimeAbi"));
        if (!QDir().mkpath(QDir(otherRoot).filePath(QStringLiteral("android-runtime"))) ||
            !otherMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            otherMetadata.write(QJsonDocument(otherObject).toJson()) < 0) return 1;
        otherMetadata.close();
        if (!standalonePaths(otherId, otherId, otherRoot, otherStorage, &paths, &error, shared, &runtimeError) ||
            paths.androidRoot != QDir(otherRoot).filePath(QStringLiteral("android-runtime")) ||
            QFileInfo::exists(QDir(paths.androidRoot).filePath(QStringLiteral("app/webview/webview.apk")))) return 1;
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate) || marker.write("2\n") != 2) return 1;
        marker.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            runtimeError != StandaloneRuntimeError::Incompatible || error.isEmpty()) return 1;
        if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate) || marker.write("invalid\n") != 8) return 1;
        marker.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            runtimeError != StandaloneRuntimeError::Unavailable || error.isEmpty()) return 1;
        object.insert(QStringLiteral("sharedRuntimeAbi"), QJsonValue(1));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(QJsonDocument(object).toJson()) < 0) return 1;
        file.close();
        if (standalonePaths(appId, appId, root, storage, &paths, &error, shared, &runtimeError) ||
            runtimeError != StandaloneRuntimeError::None || error.isEmpty()) return 1;
        if (!metadata(appId, package, QStringLiteral("zh-Hant-TW"), QStringLiteral("dark"), QStringLiteral("55.5,37.5"))) return 1;
    }
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
