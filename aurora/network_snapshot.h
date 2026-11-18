#ifndef AURORA_NETWORK_SNAPSHOT_H
#define AURORA_NETWORK_SNAPSHOT_H

#include "../core/include/android_compatibility_layer_network.h"

inline int auroraNetworkSnapshot(bool hasKnownConfiguration, int activeCount,
                                 int activeTransport, bool online)
{
    if (activeCount > 0 || online)
        return ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED |
               (activeCount == 1 ? activeTransport : 0) |
               (online ? ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET : 0);
    return hasKnownConfiguration ? ANDROID_COMPATIBILITY_LAYER_NETWORK_OFFLINE : ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN;
}

inline int auroraNetworkCapabilities(int snapshot, bool routeValid, bool routeConnected,
                                     int routeTransport, bool routeOnline, bool routeFavorite)
{
    const int transport = snapshot & ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_MASK;
    if ((snapshot & ANDROID_COMPATIBILITY_LAYER_NETWORK_STATE_MASK) != ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED ||
        !routeValid || !routeConnected || !transport || routeTransport != transport)
        return snapshot;
    // ConnMan stays "ready" when its own online check is not configured, so a ready route
    // plus Qt's successful probe is enough; WorkManager requires VALIDATED to run downloads.
    (void)routeOnline;
    if (snapshot & ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET)
        snapshot |= ANDROID_COMPATIBILITY_LAYER_NETWORK_VALIDATED;
    if ((transport == ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI && routeFavorite) ||
        transport == ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_CELLULAR ||
        transport == ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_ETHERNET)
        snapshot |= ANDROID_COMPATIBILITY_LAYER_NETWORK_TRUSTED;
    return snapshot;
}

#endif
