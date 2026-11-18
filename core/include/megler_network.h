#ifndef MEGLER_NETWORK_H
#define MEGLER_NETWORK_H

#include "megler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A snapshot, not a guarantee of internet reachability. Zero is unknown. */
enum {
    MEGLER_NETWORK_UNKNOWN = 0,
    MEGLER_NETWORK_OFFLINE = 1,
    MEGLER_NETWORK_CONNECTED = 2,
    MEGLER_NETWORK_LEGACY = 3,
    MEGLER_NETWORK_STATE_MASK = 3,
    MEGLER_NETWORK_TRANSPORT_WIFI = 4,
    MEGLER_NETWORK_TRANSPORT_CELLULAR = 8,
    MEGLER_NETWORK_TRANSPORT_ETHERNET = 12,
    MEGLER_NETWORK_TRANSPORT_BLUETOOTH = 16,
    MEGLER_NETWORK_TRANSPORT_MASK = 28,
    MEGLER_NETWORK_INTERNET = 32,
    MEGLER_NETWORK_VALIDATED = 64,
    MEGLER_NETWORK_TRUSTED = 128
};

/* May be called on any thread; pass UNKNOWN when the host stops. */
MEGLER_API void megler_network_publish(int snapshot);

#ifdef __cplusplus
}
#endif
#endif
