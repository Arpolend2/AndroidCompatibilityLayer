#ifndef ACL_STANDALONE_DIAGNOSTIC_REGISTRY_H
#define ACL_STANDALONE_DIAGNOSTIC_REGISTRY_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>

namespace StandaloneDiagnosticRegistry {

inline QString appIdFromExecutable(const QString &executable)
{
    static const QRegularExpression installed(QStringLiteral(
        "\\A/opt/app/(ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24})/"
        "[A-Za-z0-9][A-Za-z0-9.+~_-]*/bin/\\1\\z"));
    static const QRegularExpression direct(QStringLiteral(
        "\\A/usr/bin/(ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24})\\z"));
    QRegularExpressionMatch match = direct.match(executable);
    if (!match.hasMatch()) match = installed.match(executable);
    return match.hasMatch() ? match.captured(1) : QString();
}

inline bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}\\z"));
    return pattern.match(id).hasMatch();
}

inline bool validPackage(const QString &package)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    return package.size() <= 255 && pattern.match(package).hasMatch();
}

inline QString directory(const QString &root)
{
    return QDir(root).filePath(QStringLiteral("standalone-diagnostic-registry"));
}

inline QString lookup(const QString &root, const QString &id)
{
    if (root.isEmpty() || !validId(id) || QFileInfo(root).isSymLink() ||
        QFileInfo(directory(root)).isSymLink()) return QString();
    const QString path = QDir(directory(root)).filePath(id);
    if (QFileInfo(path).isSymLink()) return QString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    const QByteArray data = file.read(256);
    const QString package = QString::fromLatin1(data);
    return file.error() == QFile::NoError && file.atEnd() && validPackage(package)
        ? package : QString();
}

inline bool save(const QString &root, const QString &rpmFile, const QString &package)
{
    static const QRegularExpression filename(QStringLiteral(
        "\\A(ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24})-1\\.0\\.0-1\\.(?:aarch64|x86_64)\\.rpm\\z"));
    const QFileInfo rpm(rpmFile);
    const QRegularExpressionMatch match = filename.match(rpm.fileName());
    if (root.isEmpty() || !QFileInfo(root).isDir() || QFileInfo(root).isSymLink() ||
        !rpm.isFile() || rpm.isSymLink() || !match.hasMatch() || !validPackage(package))
        return false;
    const QString dir = directory(root);
    if (QFileInfo(dir).isSymLink() || !QDir().mkpath(dir)) return false;
    QLockFile lock(QDir(dir).filePath(QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) return false;
    const QString id = match.captured(1);
    const QString target = QDir(dir).filePath(id);
    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink())
        return lookup(root, id) == package;
    const QByteArray value = package.toLatin1();
    QSaveFile file(target);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size() && file.commit();
}
}
#endif
