#ifndef AURORA_NETWORK_SNAPSHOT_H
#define AURORA_NETWORK_SNAPSHOT_H

#include "../core/include/megler_network.h"

inline int auroraNetworkSnapshot(bool hasKnownConfiguration, int activeCount,
                                 int activeTransport, bool online)
{
    if (activeCount > 0 || online)
        return MEGLER_NETWORK_CONNECTED |
               (activeCount == 1 ? activeTransport : 0) |
               (online ? MEGLER_NETWORK_INTERNET : 0);
    return hasKnownConfiguration ? MEGLER_NETWORK_OFFLINE : MEGLER_NETWORK_UNKNOWN;
}

inline int auroraNetworkCapabilities(int snapshot, bool routeValid, bool routeConnected,
                                     int routeTransport, bool routeOnline, bool routeFavorite)
{
    const int transport = snapshot & MEGLER_NETWORK_TRANSPORT_MASK;
    if ((snapshot & MEGLER_NETWORK_STATE_MASK) != MEGLER_NETWORK_CONNECTED ||
        !routeValid || !routeConnected || !transport || routeTransport != transport)
        return snapshot;
    // ConnMan stays "ready" when its own online check is not configured, so a ready route
    // plus Qt's successful probe is enough; WorkManager requires VALIDATED to run downloads.
    (void)routeOnline;
    if (snapshot & MEGLER_NETWORK_INTERNET)
        snapshot |= MEGLER_NETWORK_VALIDATED;
    if ((transport == MEGLER_NETWORK_TRANSPORT_WIFI && routeFavorite) ||
        transport == MEGLER_NETWORK_TRANSPORT_CELLULAR ||
        transport == MEGLER_NETWORK_TRANSPORT_ETHERNET)
        snapshot |= MEGLER_NETWORK_TRUSTED;
    return snapshot;
}

#endif
