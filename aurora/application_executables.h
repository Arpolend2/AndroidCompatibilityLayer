#ifndef ACL_APPLICATION_EXECUTABLES_H
#define ACL_APPLICATION_EXECUTABLES_H

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace ApplicationExecutables {

inline QString worker()
{
    // Sailjail's private-bin exposes the launched executable, not its siblings.
    return QCoreApplication::applicationFilePath();
}

inline QString standalonePayload(const QString &applicationDataDirectory)
{
    return QDir(applicationDataDirectory).absoluteFilePath(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager.runtime"));
}

inline QString manager()
{
    const QFileInfo current(QCoreApplication::applicationFilePath());
    if (current.fileName() != QLatin1String("ru.androidcompatibilitylayer.appmanager.runtime"))
        return current.absoluteFilePath();

    return current.absoluteDir().absoluteFilePath(
        QStringLiteral("ru.androidcompatibilitylayer.appmanager"));
}

}

#endif
