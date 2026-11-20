#include "standalone_app_mapping.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
QString path(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral(".last-created-app-id"));
}
bool valid(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}\\z"));
    return pattern.match(id).hasMatch();
}
}

bool StandaloneAppMapping::save(const QString &directory, const QString &rpmFile)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\A(ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24})-1\\.0\\.0-1\\.(?:aarch64|armv7hl|x86_64)\\.rpm\\z"));
    const QFileInfo dir(directory), rpm(rpmFile);
    const QRegularExpressionMatch match = pattern.match(rpm.fileName());
    if (!dir.isDir() || dir.isSymLink() || rpm.isSymLink() || !rpm.isFile() ||
        QDir::cleanPath(rpm.absolutePath()) != QDir::cleanPath(dir.absoluteFilePath()) ||
        !match.hasMatch() || QFileInfo(path(directory)).isSymLink()) return false;
    const QByteArray id = match.captured(1).toLatin1();
    QSaveFile file(path(directory));
    return file.open(QIODevice::WriteOnly) && file.write(id) == id.size() && file.commit();
}

QString StandaloneAppMapping::read(const QString &directory)
{
    if (QFileInfo(directory).isSymLink() || QFileInfo(path(directory)).isSymLink()) return QString();
    QFile file(path(directory));
    if (!file.open(QIODevice::ReadOnly)) return QString();
    const QByteArray bytes = file.read(128);
    const QString id = QString::fromLatin1(bytes);
    return file.error() == QFile::NoError && file.atEnd() && valid(id) ? id : QString();
}
