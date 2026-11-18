#include "../network_host.h"
#include <QCoreApplication>
#include <cassert>

static int snapshot = -1;
extern "C" void android_compatibility_layer_network_publish(int value) { snapshot = value; }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ConnmanServices services;
    ConnmanService wifi;
    wifi.properties.insert(QStringLiteral("State"), QStringLiteral("online"));
    wifi.properties.insert(QStringLiteral("Type"), QStringLiteral("wifi"));
    wifi.properties.insert(QStringLiteral("Favorite"), true);
    services.append(wifi);
    ConnmanRoute route = connmanRoute(services);
    assert(route.valid && route.online && route.favorite && route.type == QLatin1String("wifi"));
    ConnmanService idle;
    idle.properties.insert(QStringLiteral("State"), QStringLiteral("idle"));
    services.append(idle);
    assert(connmanRoute(services).valid);
    services[1].properties[QStringLiteral("State")] = QStringLiteral("ready");
    assert(!connmanRoute(services).valid);
    services.clear();
    assert(!connmanRoute(services).valid);

    AuroraNetworkHost host;
    assert(snapshot >= 0);
    host.shutdown();
    assert(snapshot == ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN);
    host.shutdown();
    QCoreApplication::processEvents();
    assert(snapshot == ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN);
}
