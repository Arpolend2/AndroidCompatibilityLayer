#ifndef MEGLER_AURORA_EXTERNAL_APK_PATH_H
#define MEGLER_AURORA_EXTERNAL_APK_PATH_H

#include <QString>

bool externalApkPath(const QString &argument, QString *path, QString *error, bool allowBundles = false);

#endif
