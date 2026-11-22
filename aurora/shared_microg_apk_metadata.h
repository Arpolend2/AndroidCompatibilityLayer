#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_APK_METADATA_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_APK_METADATA_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../core/include/megler.h"
#include "../core/src/apk/mg_apk.h"

inline QByteArray readSharedMicrogApkMetadata(const QString &apkPath,
                                             const QString &expectedPackage, QString *error)
{
    if (error) error->clear();
    const QFileInfo apkInfo(apkPath);
    if (apkPath.isEmpty() || !apkInfo.isFile() || !apkInfo.isReadable()) {
        if (error) *error = QStringLiteral("The running client's APK is unavailable: %1").arg(apkPath);
        return QByteArray();
    }
    const QByteArray apkBytes = QFile::encodeName(apkInfo.absoluteFilePath());
    mg_apk_info info = {};
    if (!mg_apk_read_info(apkBytes.constData(), &info)) {
        const QString detail = QString::fromUtf8(info.error);
        mg_apk_info_free(&info);
        if (error) *error = detail.isEmpty()
            ? QStringLiteral("The running client's APK could not be inspected.") : detail;
        return QByteArray();
    }
    if (QString::fromUtf8(info.package) != expectedPackage) {
        mg_apk_info_free(&info);
        if (error) *error = QStringLiteral("The running APK package does not match the identified caller.");
        return QByteArray();
    }
    QJsonObject metadata;
    metadata.insert(QStringLiteral("package"), QString::fromUtf8(info.package));
    metadata.insert(QStringLiteral("activity"), QString::fromUtf8(info.activity));
    metadata.insert(QStringLiteral("label"), QString::fromUtf8(info.label));
    metadata.insert(QStringLiteral("versionName"), QString::fromUtf8(info.version_name));
    metadata.insert(QStringLiteral("versionCode"), QString::number(info.version_code));
    mg_apk_info_free(&info);

    QTemporaryDir temporary;
    if (!temporary.isValid() ||
        !megler_apk_describe_activities(apkBytes.constData(), nullptr,
            QFile::encodeName(temporary.path()).constData())) {
        if (error) *error = QStringLiteral("The running client's APK activities or certificate could not be read.");
        return QByteArray();
    }
    QFile activities(QDir(temporary.path()).filePath(QStringLiteral("activities.json")));
    if (!activities.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read the running client's APK description: %1")
            .arg(activities.errorString());
        return QByteArray();
    }
    if (activities.size() <= 0 || activities.size() > 1024 * 1024) {
        if (error) *error = QStringLiteral("The running client's APK description exceeds the metadata limit.");
        return QByteArray();
    }
    const QByteArray contents = activities.readAll();
    if (activities.error() != QFile::NoError) {
        if (error) *error = QStringLiteral("Cannot read the running client's APK description: %1")
            .arg(activities.errorString());
        return QByteArray();
    }
    QJsonParseError parseError;
    const QJsonDocument description = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !description.isObject() ||
        description.object().value(QStringLiteral("package")).toString() != expectedPackage) {
        if (error) *error = QStringLiteral("The running client's APK description is invalid.");
        return QByteArray();
    }
    metadata.insert(QStringLiteral("activities"), description.object());
    const QByteArray payload = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (payload.size() > 1024 * 1024) {
        if (error) *error = QStringLiteral("The running client's APK metadata exceeds the size limit.");
        return QByteArray();
    }
    return payload;
}

#endif
