#include "external_apk_path.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QUrl>

bool externalApkPath(const QString &argument, QString *path, QString *error, bool allowBundles)
{
    if (path) path->clear();
    if (error) error->clear();

    QString local;
    if (argument.startsWith(QLatin1Char('/'))) {
        local = argument;
    } else {
        const QUrl url(argument, QUrl::StrictMode);
        if (!url.isValid() || url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0 ||
            !url.host().isEmpty() || url.hasQuery() || url.hasFragment() || !url.isLocalFile()) {
            if (error) *error = QObject::tr("Only local file URLs are supported.");
            return false;
        }
        local = url.toLocalFile();
    }
    if (local.contains(QChar::Null) || !local.startsWith(QLatin1Char('/')) ||
        local.startsWith(QLatin1String("//")) || QDir::cleanPath(local) != local ||
        QFileInfo(local).absolutePath() == QLatin1String("/") ||
        !(local.endsWith(QLatin1String(".apk"), Qt::CaseInsensitive) ||
          (allowBundles && (local.endsWith(QLatin1String(".xapk"), Qt::CaseInsensitive) ||
                            local.endsWith(QLatin1String(".apks"), Qt::CaseInsensitive))))) {
        if (error) *error = QObject::tr("The selected file is not a valid local APK or bundle path.");
        return false;
    }
    if (path) *path = local;
    return true;
}
