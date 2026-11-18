#ifndef AURORA_NETWORK_CONNMAN_H
#define AURORA_NETWORK_CONNMAN_H

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QList>
#include <QVariantMap>

struct ConnmanService {
    QDBusObjectPath path;
    QVariantMap properties;
};
typedef QList<ConnmanService> ConnmanServices;
Q_DECLARE_METATYPE(ConnmanService)
Q_DECLARE_METATYPE(ConnmanServices)

inline QDBusArgument &operator<<(QDBusArgument &argument, const ConnmanService &service)
{
    argument.beginStructure();
    argument << service.path << service.properties;
    argument.endStructure();
    return argument;
}

inline const QDBusArgument &operator>>(const QDBusArgument &argument, ConnmanService &service)
{
    argument.beginStructure();
    argument >> service.path >> service.properties;
    argument.endStructure();
    return argument;
}

struct ConnmanRoute {
    bool valid = false;
    bool online = false;
    bool favorite = false;
    QString type;
};

inline ConnmanRoute connmanRoute(const ConnmanServices &services)
{
    ConnmanRoute route;
    for (const ConnmanService &service : services) {
        const QString state = service.properties.value(QStringLiteral("State")).toString();
        if (state != QLatin1String("ready") && state != QLatin1String("online"))
            continue;
        if (route.valid)
            return ConnmanRoute();
        route.valid = true;
        route.online = state == QLatin1String("online");
        route.favorite = service.properties.value(QStringLiteral("Favorite")).toBool();
        route.type = service.properties.value(QStringLiteral("Type")).toString();
    }
    return route;
}

#endif
