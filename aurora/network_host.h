#ifndef AURORA_NETWORK_HOST_H
#define AURORA_NETWORK_HOST_H

#include "../core/include/android_compatibility_layer_network.h"
#include "network_connman.h"

#include <QObject>
#include <QNetworkConfigurationManager>
#include <QTimer>

class AuroraNetworkHost : public QObject {
public:
    explicit AuroraNetworkHost(QObject *parent = nullptr);
    ~AuroraNetworkHost();
    void shutdown();

private:
    void refresh();
    void invalidateRoute();
    void requestRoute();
    QNetworkConfigurationManager m_manager;
    QTimer m_routeTimer;
    ConnmanRoute m_route;
    QString m_routeError;
    unsigned int m_routeGeneration = 0;
    bool m_routeRequestPending = false;
    bool m_stopped = false;
    int m_lastSnapshot = -1;
};

#endif
