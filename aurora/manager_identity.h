#ifndef MEGLER_MANAGER_IDENTITY_H
#define MEGLER_MANAGER_IDENTITY_H

#include <QRegularExpression>
#include <QString>

constexpr int MaxAppManagerIdentityKeyLength = 256;

inline bool isTrustedAppManagerExecutable(const QString &executable)
{
    static const QRegularExpression installedPath(QStringLiteral(
        "\\A/opt/app/ru\\.androidcompatibilitylayer\\.appmanager/"
        "[A-Za-z0-9][A-Za-z0-9.+~_-]*/bin/ru\\.androidcompatibilitylayer\\.appmanager\\z"));
    return executable == QLatin1String("/usr/bin/ru.androidcompatibilitylayer.appmanager") ||
           installedPath.match(executable).hasMatch();
}

inline bool isTrustedLogSenderExecutable(const QString &executable)
{
    if (isTrustedAppManagerExecutable(executable))
        return true;
    static const QRegularExpression standalonePath(QStringLiteral(
        "\\A(?:/usr/bin/ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}"
        "|/opt/app/(ru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24})/"
        "[A-Za-z0-9][A-Za-z0-9.+~_-]*/bin/\\1)\\z"));
    return standalonePath.match(executable).hasMatch();
}

#endif
