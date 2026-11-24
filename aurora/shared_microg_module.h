#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_MODULE_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_MODULE_H

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace SharedMicrogModule {
inline QString path(const QString &sharedRoot)
{
    return sharedRoot.isEmpty() ? QString() :
        QDir(sharedRoot).filePath(QStringLiteral("microg/GmsCore.apk"));
}

inline bool publish(const QString &source, const QString &sharedRoot, QString *error,
                    QByteArray *digest = nullptr)
{
    if (error) error->clear();
    if (digest) digest->clear();
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString destination = path(sharedRoot);
    const QFileInfo sourceInfo(source), targetInfo(destination);
    if (destination.isEmpty() || !sourceInfo.isFile() || sourceInfo.isSymLink() ||
        sourceInfo.size() <= 0 || sourceInfo.size() > 512LL * 1024 * 1024 ||
        targetInfo.isSymLink())
        return fail(QStringLiteral("The shared microG module APK path is invalid."));
    const QString directory = targetInfo.absolutePath();
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink())
        return fail(QStringLiteral("The shared microG module directory is unavailable."));

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot read the installed microG APK."));
    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    if (!sourceHash.addData(&input))
        return fail(QStringLiteral("Cannot hash the installed microG APK."));
    const QByteArray expected = sourceHash.result();
    if (targetInfo.isFile() && targetInfo.size() == sourceInfo.size()) {
        QFile current(destination);
        QCryptographicHash currentHash(QCryptographicHash::Sha256);
        if (!current.open(QIODevice::ReadOnly) || !currentHash.addData(&current))
            return fail(QStringLiteral("Cannot read the shared microG module APK."));
        if (currentHash.result() == expected) {
            if (digest) *digest = expected;
            return true;
        }
    }
    if (!input.seek(0))
        return fail(QStringLiteral("Cannot reread the installed microG APK."));
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("Cannot stage the shared microG module APK."));
    QCryptographicHash copiedHash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!input.atEnd()) {
        const QByteArray bytes = input.read(65536);
        if (bytes.isEmpty() || copied > sourceInfo.size() - bytes.size() ||
            output.write(bytes) != bytes.size())
            return fail(QStringLiteral("Cannot copy the installed microG APK."));
        copied += bytes.size();
        copiedHash.addData(bytes);
    }
    if (input.error() != QFile::NoError || copied != sourceInfo.size() ||
        copiedHash.result() != expected)
        return fail(QStringLiteral("The installed microG APK changed during publication."));
    if (!output.commit())
        return fail(QStringLiteral("Cannot publish the shared microG module APK."));
    if (digest) *digest = expected;
    return true;
}
}

#endif
