#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace AuroraBluetooth {

bool addressValue(const QString &address, unsigned long long *value);
int addressType(const QString &type);
QByteArray advertisementData(const QString &name, const QStringList &uuids, int txPower);

}
