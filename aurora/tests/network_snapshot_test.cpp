#include "../network_snapshot.h"

#include <cassert>

int main()
{
    assert(auroraNetworkSnapshot(false, 0, 0, false) == MEGLER_NETWORK_UNKNOWN);
    assert(auroraNetworkSnapshot(true, 0, 0, false) == MEGLER_NETWORK_OFFLINE);
    assert(auroraNetworkSnapshot(true, 1, MEGLER_NETWORK_TRANSPORT_WIFI, false) ==
           (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI));
    assert(auroraNetworkSnapshot(true, 1, MEGLER_NETWORK_TRANSPORT_CELLULAR, true) ==
           (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_CELLULAR | MEGLER_NETWORK_INTERNET));
    assert(auroraNetworkSnapshot(true, 2, MEGLER_NETWORK_TRANSPORT_WIFI, true) ==
           (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_INTERNET));
    assert(auroraNetworkSnapshot(false, 0, 0, true) ==
           (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_INTERNET));

    const int wifi = MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI | MEGLER_NETWORK_INTERNET;
    assert(auroraNetworkCapabilities(wifi, true, true, MEGLER_NETWORK_TRANSPORT_WIFI, true, true) ==
           (wifi | MEGLER_NETWORK_VALIDATED | MEGLER_NETWORK_TRUSTED));
    assert(auroraNetworkCapabilities(wifi, true, true, MEGLER_NETWORK_TRANSPORT_WIFI, false, true) ==
           (wifi | MEGLER_NETWORK_VALIDATED | MEGLER_NETWORK_TRUSTED));
    assert(auroraNetworkCapabilities(MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI, true, true,
                                     MEGLER_NETWORK_TRANSPORT_WIFI, true, true) ==
           (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI | MEGLER_NETWORK_TRUSTED));
    assert(auroraNetworkCapabilities(wifi, true, true, MEGLER_NETWORK_TRANSPORT_WIFI, true, false) ==
           (wifi | MEGLER_NETWORK_VALIDATED));
    assert(auroraNetworkCapabilities(wifi, false, true, MEGLER_NETWORK_TRANSPORT_WIFI, true, true) == wifi);
    assert(auroraNetworkCapabilities(wifi, true, false, MEGLER_NETWORK_TRANSPORT_WIFI, true, true) == wifi);
    assert(auroraNetworkCapabilities(wifi, true, true, MEGLER_NETWORK_TRANSPORT_CELLULAR, true, true) == wifi);
    assert(auroraNetworkCapabilities(MEGLER_NETWORK_OFFLINE, true, true, MEGLER_NETWORK_TRANSPORT_WIFI, true, true) ==
           MEGLER_NETWORK_OFFLINE);
}
