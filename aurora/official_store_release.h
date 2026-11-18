#ifndef MEGLER_AURORA_OFFICIAL_STORE_RELEASE_H
#define MEGLER_AURORA_OFFICIAL_STORE_RELEASE_H

#include <QByteArray>
#include <QString>
#include <QUrl>

struct OfficialStoreRelease {
    QString version;
    QUrl apkUrl;
};

bool parseOfficialStoreRelease(const QByteArray &json, OfficialStoreRelease *release, QString *error);

#endif
