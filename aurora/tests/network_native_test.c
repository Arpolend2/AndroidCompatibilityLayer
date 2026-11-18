#include "../../core/src/drivers/mg_java_wifi.c"

#include <assert.h>
#include <pthread.h>

i32 mg_atomic_cas(volatile i32 *p, i32 expect, i32 value)
{
    return __sync_val_compare_and_swap(p, expect, value);
}

i32 mg_atomic_xchg(volatile i32 *p, i32 value)
{
    return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}

static int read_snapshot(void)
{
    mg_jcall call = {0};
    Network_state(&call);
    return call.ret.i;
}

static void *publish_many(void *unused)
{
    int i;
    (void)unused;
    for (i = 0; i < 100000; ++i)
        android_compatibility_layer_network_publish((i & 1) ? ANDROID_COMPATIBILITY_LAYER_NETWORK_OFFLINE :
            ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI | ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    int i;
    assert(read_snapshot() == ANDROID_COMPATIBILITY_LAYER_NETWORK_LEGACY);
    android_compatibility_layer_network_publish(ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN);
    assert(read_snapshot() == ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN);
    android_compatibility_layer_network_publish(ANDROID_COMPATIBILITY_LAYER_NETWORK_OFFLINE | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI);
    assert(read_snapshot() == ANDROID_COMPATIBILITY_LAYER_NETWORK_OFFLINE);
    android_compatibility_layer_network_publish(ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_CELLULAR | ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET);
    assert(read_snapshot() == (ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_CELLULAR | ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET));
    android_compatibility_layer_network_publish(ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI |
                           ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET | ANDROID_COMPATIBILITY_LAYER_NETWORK_VALIDATED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRUSTED);
    assert(read_snapshot() == (ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI |
                               ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET | ANDROID_COMPATIBILITY_LAYER_NETWORK_VALIDATED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRUSTED));
    android_compatibility_layer_network_publish(0x100);
    assert(read_snapshot() == ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN);
    assert(pthread_create(&thread, NULL, publish_many, NULL) == 0);
    for (i = 0; i < 100000; ++i) {
        int s = read_snapshot();
        assert(s == ANDROID_COMPATIBILITY_LAYER_NETWORK_UNKNOWN || s == ANDROID_COMPATIBILITY_LAYER_NETWORK_OFFLINE ||
               s == (ANDROID_COMPATIBILITY_LAYER_NETWORK_CONNECTED | ANDROID_COMPATIBILITY_LAYER_NETWORK_TRANSPORT_WIFI | ANDROID_COMPATIBILITY_LAYER_NETWORK_INTERNET));
    }
    assert(pthread_join(thread, NULL) == 0);
    return 0;
}
