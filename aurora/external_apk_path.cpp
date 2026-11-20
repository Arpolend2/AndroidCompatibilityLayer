#include "external_apk_path.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QObject>
#include <QUrl>

namespace {
bool hexDigit(QChar c)
{
    const ushort v = c.unicode();
    return (v >= '0' && v <= '9') || (v >= 'a' && v <= 'f') || (v >= 'A' && v <= 'F');
}

bool hasValidPercentEscapes(const QString &argument)
{
    for (int i = 0; i < argument.size(); ++i) {
        if (argument.at(i) != QLatin1Char('%')) continue;
        if (i + 2 >= argument.size() || !hexDigit(argument.at(i + 1)) || !hexDigit(argument.at(i + 2)))
            return false;
        i += 2;
    }
    return true;
}

void logMalformedArgument(const QString &argument)
{
    bool badEscape = false;
    bool whitespace = false;
    bool nonAscii = false;
    for (int i = 0; i < argument.size(); ++i) {
        const QChar c = argument.at(i);
        whitespace |= c.isSpace();
        nonAscii |= c.unicode() > 127;
        if (c == QLatin1Char('%') &&
            (i + 2 >= argument.size() || !hexDigit(argument.at(i + 1)) || !hexDigit(argument.at(i + 2))))
            badEscape = true;
    }
    const char *shape = argument.startsWith(QLatin1String("file:"), Qt::CaseInsensitive) ? "file-uri"
        : argument.startsWith(QLatin1Char('"')) || argument.startsWith(QLatin1Char('\'')) ? "quoted"
        : argument.startsWith(QLatin1Char('[')) ? "bracketed"
        : "other";
    qInfo("APK path validation: malformed shape=%s whitespace=%d invalid-escape=%d non-ascii=%d",
          shape, int(whitespace), int(badEscape), int(nonAscii));
}
}

bool externalApkPath(const QString &argument, QString *path, QString *error, bool allowBundles)
{
    if (path) path->clear();
    if (error) error->clear();

    QString local;
    if (argument.startsWith(QLatin1Char('/'))) {
        local = argument;
    } else {
        QUrl url(argument, QUrl::StrictMode);
        if (!url.isValid() && argument.startsWith(QLatin1String("file:"), Qt::CaseInsensitive) &&
            hasValidPercentEscapes(argument)) {
            const QUrl tolerant(argument, QUrl::TolerantMode);
            if (tolerant.isValid() && tolerant.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) == 0 &&
                tolerant.host().isEmpty() && !tolerant.hasQuery() && !tolerant.hasFragment() &&
                tolerant.isLocalFile()) {
                url = tolerant;
                qInfo("APK path validation: accepted unescaped local file URL");
            }
        }
        if (!url.isValid() || url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0 ||
            !url.host().isEmpty() || url.hasQuery() || url.hasFragment() || !url.isLocalFile()) {
            const char *reason = "not-local-file-url";
            if (!url.isValid()) reason = "malformed-url";
            else if (url.scheme().isEmpty()) reason = "missing-scheme";
            else if (url.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) == 0)
                reason = "content-uri";
            else if (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0 ||
                     url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)
                reason = "web-url";
            else if (url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0)
                reason = "unsupported-scheme";
            else if (!url.host().isEmpty()) reason = "file-url-has-host";
            else if (url.hasQuery() || url.hasFragment()) reason = "file-url-has-query-or-fragment";
            qInfo("APK path validation: rejected reason=%s", reason);
            if (!url.isValid()) logMalformedArgument(argument);
            if (error) *error = QObject::tr("Only local file URLs are supported.");
            return false;
        }
        local = url.toLocalFile();
    }
    if (local.contains(QChar::Null) || !local.startsWith(QLatin1Char('/')) ||
        local.startsWith(QLatin1String("//")) || QDir::cleanPath(local) != local ||
        QFileInfo(local).absolutePath() == QLatin1String("/") ||
        !(local.endsWith(QLatin1String(".apk"), Qt::CaseInsensitive) ||
          (allowBundles && (local.endsWith(QLatin1String(".xapk"), Qt::CaseInsensitive) ||
                            local.endsWith(QLatin1String(".apks"), Qt::CaseInsensitive))))) {
        const char *reason = "unsupported-extension";
        if (local.contains(QChar::Null)) reason = "nul-character";
        else if (!local.startsWith(QLatin1Char('/'))) reason = "not-absolute";
        else if (local.startsWith(QLatin1String("//"))) reason = "network-path";
        else if (QDir::cleanPath(local) != local) reason = "unclean-path";
        else if (QFileInfo(local).absolutePath() == QLatin1String("/")) reason = "root-directory";
        qInfo("APK path validation: rejected reason=%s", reason);
        if (error) *error = QObject::tr("The selected file is not a valid local APK or bundle path.");
        return false;
    }
    if (path) *path = local;
    return true;
}
