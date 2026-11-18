#pragma once

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QMap>
#include <QVariant>
#include <QVariantMap>

namespace AuroraBluetooth {

using InterfaceProperties = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QString, InterfaceProperties>;

inline bool readManagedObjects(const QVariant &value, ManagedObjects *objects)
{
    if (!objects || value.userType() != qMetaTypeId<QDBusArgument>()) return false;

    const QDBusArgument *argument = static_cast<const QDBusArgument *>(value.constData());
    if (argument->currentType() != QDBusArgument::MapType) return false;

    ManagedObjects parsed;
    argument->beginMap();
    while (!argument->atEnd()) {
        argument->beginMapEntry();
        QDBusObjectPath path;
        *argument >> path;
        InterfaceProperties interfaces;
        argument->beginMap();
        while (!argument->atEnd()) {
            argument->beginMapEntry();
            QString interfaceName;
            *argument >> interfaceName;
            QVariantMap properties;
            argument->beginMap();
            while (!argument->atEnd()) {
                argument->beginMapEntry();
                QString name;
                QDBusVariant value;
                *argument >> name >> value;
                argument->endMapEntry();
                properties.insert(name, QVariant::fromValue(value));
            }
            argument->endMap();
            argument->endMapEntry();
            interfaces.insert(interfaceName, properties);
        }
        argument->endMap();
        argument->endMapEntry();
        parsed.insert(path.path(), interfaces);
    }
    argument->endMap();
    *objects = parsed;
    return true;
}

}
