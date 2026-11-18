#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_APP_STORAGE_LOCK_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_APP_STORAGE_LOCK_H

#include <QDir>
#include <QCryptographicHash>
#include <QString>

inline QString appStorageLockPath(const QString &root, const QString &package)
{
    const QByteArray hash = QCryptographicHash::hash(package.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(root).filePath(QStringLiteral(".running-") + QString::fromLatin1(hash) + QStringLiteral(".lock"));
}

#endif
