#include "official_store_release.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>

bool parseOfficialStoreRelease(const QByteArray &json, OfficialStoreRelease *release, QString *error)
{
    if (release) *release = OfficialStoreRelease();
    if (error) error->clear();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!release || parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QObject::tr("Invalid Aurora Store release response.");
        return false;
    }

    const QJsonObject object = document.object();
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    static const QRegularExpression versionPattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+){1,3}$"));
    if (!versionPattern.match(tag).hasMatch() ||
        object.value(QStringLiteral("upcoming_release")).toBool(true)) {
        if (error) *error = QObject::tr("The Aurora Store release is not a supported stable version.");
        return false;
    }

    const QString filename = QStringLiteral("AuroraStore-%1.apk").arg(tag);
    const QRegularExpression linkPattern(
        QStringLiteral("\\[%1\\]\\((/uploads/[a-f0-9]{32}/%1)\\)")
            .arg(QRegularExpression::escape(filename)));
    QRegularExpressionMatchIterator matches =
        linkPattern.globalMatch(object.value(QStringLiteral("description")).toString());
    if (!matches.hasNext()) {
        if (error) *error = QObject::tr("The official Aurora Store release has no supported APK.");
        return false;
    }
    const QString path = matches.next().captured(1);
    if (matches.hasNext()) {
        if (error) *error = QObject::tr("The official Aurora Store release has ambiguous APK links.");
        return false;
    }

    release->version = tag;
    release->apkUrl = QUrl(QStringLiteral("https://gitlab.com/api/v4/projects/6922885") + path,
                           QUrl::StrictMode);
    return true;
}
