#ifndef MEGLER_AURORA_STANDALONE_PATHS_H
#define MEGLER_AURORA_STANDALONE_PATHS_H

#include <QString>
#include <QStringList>

struct StandalonePaths {
    QString androidRoot;
    QString apk;
    QString data;
    QString lock;
    QString displayName;
    QString locale;
    QString theme;
    QString location;
    QStringList deniedPermissions;
};

bool standalonePaths(const QString &appId, const QString &executableName,
                     const QString &resourceRoot, const QString &privateRoot,
                     StandalonePaths *paths, QString *error);

#endif
