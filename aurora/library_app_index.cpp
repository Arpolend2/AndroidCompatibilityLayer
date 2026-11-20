#include "library_app_index.h"
#include "../core/src/apk/mg_apk.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

void LibraryAppIndex::run()
{
    static const QRegularExpression packageName(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    if (m_root.isEmpty() || !QDir().mkpath(m_root) || QFileInfo(m_root).isSymLink() ||
        !QFileInfo(m_root).isReadable()) {
        error = tr("Cannot read the app library.");
        return;
    }
    const QFileInfoList directories = QDir(m_root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    if (directories.size() > 4096) {
        error = tr("The app library is too large to list.");
        return;
    }
    QString list = QStringLiteral("MEGLER_INSTALL_STATUS_V1\n");
    for (const QFileInfo &directory : directories) {
        const QString package = directory.fileName();
        if (package.size() > 159 || !packageName.match(package).hasMatch()) continue;
        const QString path = QDir(directory.filePath()).filePath(QStringLiteral("base.apk"));
        const QFileInfo apk(path);
        mg_apk_info info = {};
        if (!apk.isFile() || apk.isSymLink() || !mg_apk_read_info(QFile::encodeName(path).constData(), &info) ||
            QString::fromUtf8(info.package) != package) {
            error = tr("Cannot inspect app %1 in the library.").arg(package);
            return;
        }
        QString label = QString::fromUtf8(info.label);
        QString version = QString::fromUtf8(info.version_name);
        QString activity = QString::fromUtf8(info.activity);
        if (label.contains(QLatin1Char('\n')) || label.contains(QLatin1Char('\r')) || label.contains(QLatin1Char('\t')))
            label = package;
        if (version.contains(QLatin1Char('\n')) || version.contains(QLatin1Char('\r')) || version.contains(QLatin1Char('\t')))
            version.clear();
        if (activity.contains(QLatin1Char('\n')) || activity.contains(QLatin1Char('\r')) || activity.contains(QLatin1Char('\t')))
            activity.clear();
        list += package + QLatin1Char('\t') + activity + QLatin1Char('\t') + label + QLatin1Char('\t') +
                version + QLatin1Char('\t') + QString::number(static_cast<quint32>(info.version_code)) + QLatin1Char('\n');
        if (list.size() > 1024 * 1024) {
            error = tr("The app library is too large to list.");
            return;
        }
    }
    contents = list;
}
