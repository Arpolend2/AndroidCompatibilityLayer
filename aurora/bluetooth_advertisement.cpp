#include "bluetooth_advertisement.h"

#include "../core/include/megler.h"

#include <QUuid>

namespace AuroraBluetooth {
namespace {

void appendField(QByteArray &data, quint8 type, const QByteArray &value)
{
    if (value.size() > 254) return;
    data.append(char(value.size() + 1));
    data.append(char(type));
    data.append(value);
}

QByteArray reversed(const QByteArray &value)
{
    QByteArray result;
    result.reserve(value.size());
    for (int i = value.size() - 1; i >= 0; --i) result.append(value.at(i));
    return result;
}

}

bool addressValue(const QString &address, unsigned long long *value)
{
    if (!value || address.size() != 17) return false;
    unsigned long long result = 0;
    for (int i = 0; i < address.size(); ++i) {
        if (i % 3 == 2) {
            if (address.at(i) != QLatin1Char(':')) return false;
            continue;
        }
        bool ok = false;
        const uint digit = address.mid(i, 1).toUInt(&ok, 16);
        if (!ok) return false;
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

int addressType(const QString &type)
{
    if (type.compare(QStringLiteral("public"), Qt::CaseInsensitive) == 0)
        return MEGLER_BT_ADDRESS_PUBLIC;
    if (type.compare(QStringLiteral("random"), Qt::CaseInsensitive) == 0)
        return MEGLER_BT_ADDRESS_RANDOM;
    return MEGLER_BT_ADDRESS_UNKNOWN;
}

QByteArray advertisementData(const QString &name, const QStringList &uuids, int txPower)
{
    QByteArray data;
    if (!name.isEmpty()) appendField(data, 0x09, name.toUtf8().left(254));

    static const QString suffix = QStringLiteral("-0000-1000-8000-00805f9b34fb");
    for (const QString &source : uuids) {
        const QString uuid = QUuid(source).toString().mid(1, 36).toLower();
        if (uuid.isEmpty()) continue;
        if (uuid.endsWith(suffix)) {
            const QString prefix = uuid.left(8);
            bool ok = false;
            const uint value = prefix.toUInt(&ok, 16);
            if (!ok) continue;
            if (prefix.startsWith(QStringLiteral("0000"))) {
                QByteArray encoded;
                encoded.append(char(value & 0xff));
                encoded.append(char((value >> 8) & 0xff));
                appendField(data, 0x03, encoded);
            } else {
                QByteArray encoded;
                encoded.append(char(value & 0xff));
                encoded.append(char((value >> 8) & 0xff));
                encoded.append(char((value >> 16) & 0xff));
                encoded.append(char((value >> 24) & 0xff));
                appendField(data, 0x05, encoded);
            }
            continue;
        }
        const QByteArray encoded = QUuid(source).toRfc4122();
        if (encoded.size() == 16) appendField(data, 0x07, reversed(encoded));
    }

    if (txPower >= -128 && txPower <= 127) {
        QByteArray encoded(1, char(txPower));
        appendField(data, 0x0a, encoded);
    }
    return data;
}

}
