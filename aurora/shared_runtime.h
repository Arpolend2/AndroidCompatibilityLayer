#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_RUNTIME_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_RUNTIME_H

#include <QDir>
#include <QFile>
#include <QRegularExpression>

namespace SharedRuntime {
inline QString location() { return QStringLiteral("appmanager/android-runtime"); }

inline bool validVersion(const QString &version)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9][0-9]{0,8}\\z"));
    return pattern.match(version).hasMatch();
}

inline QString version(const QString &root)
{
    if (root.isEmpty()) return {};
    QFile marker(QDir(root).filePath(QStringLiteral(".standalone-abi")));
    if (!marker.open(QIODevice::ReadOnly) || marker.size() > 16) return {};
    const QString value = QString::fromLatin1(marker.read(16).trimmed());
    return validVersion(value) ? value : QString();
}
}

#endif
