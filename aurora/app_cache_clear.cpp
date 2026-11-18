#include <QCoreApplication>
#include "app_cache_clear.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace AppCacheClear {

bool request(const QString &appDataDirectory, const QString &package, QString *error)
{
    if (error) error->clear();
    const QFileInfo dataInfo(appDataDirectory);
    const QString dataPath = dataInfo.canonicalFilePath();
    const QString packageDirectory = QFileInfo(dataInfo.absolutePath()).fileName();
    static const QRegularExpression packageName(QStringLiteral("^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+$"));
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!dataInfo.isDir() || dataInfo.isSymLink() || dataPath.isEmpty() ||
        dataInfo.fileName() != QLatin1String("data") || packageDirectory != package ||
        !packageName.match(package).hasMatch())
        return fail(QCoreApplication::translate("AppCacheClear", "Invalid app data directory or package."));

    const QString markerDirectory = QDir(dataPath).filePath(QStringLiteral("clear-cache"));
    const QFileInfo markerDirectoryInfo(markerDirectory);
    if ((markerDirectoryInfo.exists() || markerDirectoryInfo.isSymLink()) &&
        (!markerDirectoryInfo.isDir() || markerDirectoryInfo.isSymLink()))
        return fail(QCoreApplication::translate("AppCacheClear", "The cache request directory is not a regular folder."));
    if (!QDir().mkpath(markerDirectory))
        return fail(QCoreApplication::translate("AppCacheClear", "Could not create the cache request directory."));

    const QString markerPath = QDir(markerDirectory).filePath(package);
    if (QFileInfo(markerPath).isSymLink())
        return fail(QCoreApplication::translate("AppCacheClear", "The cache request marker is a symbolic link."));
    QSaveFile marker(markerPath);
    if (!marker.open(QIODevice::WriteOnly) || marker.write("1", 1) != 1 || !marker.commit())
        return fail(QCoreApplication::translate("AppCacheClear", "Could not save the cache clear request."));
    return true;
}

}
