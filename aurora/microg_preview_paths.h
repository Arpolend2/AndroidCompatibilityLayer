#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_MICROG_PREVIEW_PATHS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_MICROG_PREVIEW_PATHS_H

#include <QDir>
#include <QString>

inline bool microgPreviewPaths(const QString &appData, const QString &androidRoot,
                               const QString &selectedApk, const QString &apk, const QString &data)
{
    if (appData.isEmpty() || apk.isEmpty() || data.isEmpty()) return false;
    const QDir services(QDir(appData).filePath(QStringLiteral("services")));
    const QString normalizedApk = QDir::cleanPath(apk);
    if (QDir::cleanPath(data) != QDir::cleanPath(services.filePath(QStringLiteral("data"))))
        return false;
    return normalizedApk == QDir::cleanPath(services.filePath(QStringLiteral("GmsCore.apk"))) ||
           (!androidRoot.isEmpty() &&
            normalizedApk == QDir::cleanPath(QDir(androidRoot).filePath(QStringLiteral("services/GmsCore.apk")))) ||
           (!selectedApk.isEmpty() && normalizedApk == QDir::cleanPath(selectedApk));
}

#endif
