#include "../apk_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" char *megler_apk_report(const char *path)
{
    const QString name = QFile::decodeName(path);
    const char *json = name.contains("org.example.zulu")
        ? "{\"ok\":true,\"package\":\"org.example.zulu\",\"label\":\"Zulu\"}"
        : name.contains("org.example.alpha")
        ? "{\"ok\":true,\"package\":\"org.example.alpha\",\"label\":\"alpha\"}"
        : "{\"ok\":true,\"package\":\"org.example.empty\",\"label\":\"\"}";
    return ::strdup(json);
}

extern "C" bool megler_apk_icon(const char *, uint8_t **, size_t *) { return false; }
extern "C" char *megler_apk_report_splits(const char *path, const char *splits)
{
    const QString splitDirectory = QFile::decodeName(splits);
    if (!splitDirectory.isEmpty() &&
        QFile::exists(QDir(splitDirectory).filePath(QStringLiteral("split_config.en.apk"))))
        return ::strdup("{\"ok\":true,\"package\":\"org.example.bundle\",\"label\":\"Bundle\"}");
    return megler_apk_report(path);
}
extern "C" bool megler_apk_icon_splits(const char *path, const char *splits, uint8_t **icon, size_t *size)
{
    Q_UNUSED(path);
    Q_UNUSED(splits);
    Q_UNUSED(icon);
    Q_UNUSED(size);
    return false;
}
extern "C" void megler_free(void *data) { std::free(data); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    for (const QString &package : {QStringLiteral("org.example.zulu"), QStringLiteral("org.example.alpha"),
                                   QStringLiteral("org.example.empty"), QStringLiteral("org.example.bundle")}) {
        const QString dir = QDir(root.path()).filePath(package);
        if (!QDir().mkpath(dir)) return 1;
        QFile apk(QDir(dir).filePath(QStringLiteral("base.apk")));
        if (!apk.open(QIODevice::WriteOnly) || apk.write("test") != 4) return 1;
    }
    const QString splits = QDir(root.path()).filePath(QStringLiteral("org.example.bundle/splits"));
    if (!QDir().mkpath(splits)) return 1;
    QFile split(QDir(splits).filePath(QStringLiteral("split_config.en.apk")));
    if (!split.open(QIODevice::WriteOnly) || split.write("split") != 5) return 1;
    ApkStore store(root.path());
    QString error;
    if (!store.recordLaunch(QStringLiteral("org.example.zulu"), &error) ||
        !store.recordLaunch(QStringLiteral("org.example.zulu"), &error)) return 1;
    const QVariantList apps = ApkStore(root.path()).apps();
    if (apps.size() != 4 ||
        apps.at(0).toMap().value(QStringLiteral("label")).toString() != QStringLiteral("alpha") ||
        apps.at(1).toMap().value(QStringLiteral("label")).toString() != QStringLiteral("Bundle") ||
        apps.at(2).toMap().value(QStringLiteral("label")).toString() != QStringLiteral("org.example.empty") ||
        apps.at(3).toMap().value(QStringLiteral("label")).toString() != QStringLiteral("Zulu") ||
        apps.at(3).toMap().value(QStringLiteral("launchCount")).toULongLong() != 2) return 1;
    const QString rpmDir = QDir(root.path()).filePath(QStringLiteral("org.example.zulu/standalone-rpm"));
    if (!QDir().mkpath(rpmDir)) return 1;
    const QString rpmPath = QDir(rpmDir).filePath(QStringLiteral("app.rpm"));
    QFile rpm(rpmPath);
    if (!rpm.open(QIODevice::WriteOnly) || rpm.write("rpm") != 3) return 1;
    rpm.close();
    if (!store.remove(QStringLiteral("org.example.zulu"), &error) ||
        QFile::exists(rpmPath) || !QFile::exists(QDir(root.path()).filePath(QStringLiteral("org.example.alpha/base.apk"))))
        return 1;
    return 0;
}
