#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_STANDALONE_APP_MAPPING_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_STANDALONE_APP_MAPPING_H

#include <QString>

namespace StandaloneAppMapping {
bool save(const QString &rpmDirectory, const QString &rpmFile);
QString read(const QString &rpmDirectory);
}

#endif
