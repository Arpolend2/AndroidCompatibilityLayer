#include "../bluetooth_advertisement.h"

#include "../../core/include/megler.h"

#include <cassert>
#include <cstdio>

int main()
{
    unsigned long long address = 0;
    assert(AuroraBluetooth::addressValue(QStringLiteral("AA:BB:CC:DD:EE:FF"), &address));
    assert(address == 0xAABBCCDDEEFFULL);
    assert(!AuroraBluetooth::addressValue(QStringLiteral("AA:BB:CC:DD:EE:FG"), &address));
    assert(AuroraBluetooth::addressType(QStringLiteral("public")) == MEGLER_BT_ADDRESS_PUBLIC);
    assert(AuroraBluetooth::addressType(QStringLiteral("random")) == MEGLER_BT_ADDRESS_RANDOM);
    assert(AuroraBluetooth::addressType(QStringLiteral("unknown")) == MEGLER_BT_ADDRESS_UNKNOWN);

    const QByteArray ad = AuroraBluetooth::advertisementData(
        QStringLiteral("BLE"), QStringList() << QStringLiteral("0000180f-0000-1000-8000-00805f9b34fb"),
        -42);
    assert(ad.size() == 5 + 4 + 3);
    assert(static_cast<unsigned char>(ad.at(0)) == 4);
    assert(static_cast<unsigned char>(ad.at(1)) == 0x09);
    assert(ad.mid(2, 3) == QByteArray("BLE", 3));
    assert(static_cast<unsigned char>(ad.at(5)) == 3);
    assert(static_cast<unsigned char>(ad.at(6)) == 0x03);
    assert(static_cast<unsigned char>(ad.at(7)) == 0x0f);
    assert(static_cast<unsigned char>(ad.at(8)) == 0x18);
    assert(static_cast<unsigned char>(ad.at(9)) == 2);
    assert(static_cast<unsigned char>(ad.at(10)) == 0x0a);
    assert(static_cast<signed char>(ad.at(11)) == -42);

    const QByteArray customUuid = AuroraBluetooth::advertisementData(
        QString(), QStringList() << QStringLiteral("12345678-9abc-def0-1234-56789abcdef0"), 128);
    assert(customUuid.size() == 18);
    assert(static_cast<unsigned char>(customUuid.at(0)) == 17);
    assert(static_cast<unsigned char>(customUuid.at(1)) == 0x07);
    assert(static_cast<unsigned char>(customUuid.at(2)) == 0xf0);
    assert(static_cast<unsigned char>(customUuid.at(17)) == 0x12);
    std::puts("bluetooth_advertisement_test: passed");
}
