#include "../official_store_release.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

static QByteArray response(const QString &tag, const QString &description, bool upcoming = false)
{
    QJsonObject object;
    object.insert(QStringLiteral("tag_name"), tag);
    object.insert(QStringLiteral("description"), description);
    object.insert(QStringLiteral("upcoming_release"), upcoming);
    return QJsonDocument(object).toJson();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString link = QStringLiteral("[AuroraStore-4.8.4.apk](/uploads/245eec8867a47057cbc54535f73b32ee/AuroraStore-4.8.4.apk)");
    OfficialStoreRelease release;
    QString error;
    if (!parseOfficialStoreRelease(response(QStringLiteral("4.8.4"), link), &release, &error) ||
        release.version != QStringLiteral("4.8.4") ||
        release.apkUrl.toString() != QStringLiteral("https://gitlab.com/api/v4/projects/6922885/uploads/245eec8867a47057cbc54535f73b32ee/AuroraStore-4.8.4.apk"))
        return 1;
    if (parseOfficialStoreRelease(response(QStringLiteral("4.8.4"), link, true), &release, &error) ||
        error.isEmpty() || !release.apkUrl.isEmpty()) return 1;
    if (parseOfficialStoreRelease(response(QStringLiteral("4.8.4-beta"), link), &release, &error) ||
        error.isEmpty()) return 1;
    if (parseOfficialStoreRelease(response(QStringLiteral("4.8.4"), link + link), &release, &error) ||
        error.isEmpty()) return 1;
    if (parseOfficialStoreRelease(response(QStringLiteral("4.8.4"),
            QStringLiteral("[AuroraStore-4.8.4.apk](https://example.com/a.apk)")), &release, &error) ||
        error.isEmpty()) return 1;
    if (parseOfficialStoreRelease("not json", &release, &error) || error.isEmpty()) return 1;
    if (argc > 1) {
        QFile input(QString::fromLocal8Bit(argv[1]));
        if (!input.open(QIODevice::ReadOnly) ||
            !parseOfficialStoreRelease(input.readAll(), &release, &error)) {
            std::fprintf(stderr, "Live release response rejected: %s\n", qPrintable(error));
            return 1;
        }
        std::printf("Latest Aurora Store release: %s\n", qPrintable(release.version));
    }
    std::puts("Official store release parser OK");
    return 0;
}
