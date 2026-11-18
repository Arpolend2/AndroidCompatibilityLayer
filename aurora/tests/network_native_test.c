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
        megler_network_publish((i & 1) ? MEGLER_NETWORK_OFFLINE :
            MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI | MEGLER_NETWORK_INTERNET);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    int i;
    assert(read_snapshot() == MEGLER_NETWORK_LEGACY);
    megler_network_publish(MEGLER_NETWORK_UNKNOWN);
    assert(read_snapshot() == MEGLER_NETWORK_UNKNOWN);
    megler_network_publish(MEGLER_NETWORK_OFFLINE | MEGLER_NETWORK_TRANSPORT_WIFI);
    assert(read_snapshot() == MEGLER_NETWORK_OFFLINE);
    megler_network_publish(MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_CELLULAR | MEGLER_NETWORK_INTERNET);
    assert(read_snapshot() == (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_CELLULAR | MEGLER_NETWORK_INTERNET));
    megler_network_publish(MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI |
                           MEGLER_NETWORK_INTERNET | MEGLER_NETWORK_VALIDATED | MEGLER_NETWORK_TRUSTED);
    assert(read_snapshot() == (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI |
                               MEGLER_NETWORK_INTERNET | MEGLER_NETWORK_VALIDATED | MEGLER_NETWORK_TRUSTED));
    megler_network_publish(0x100);
    assert(read_snapshot() == MEGLER_NETWORK_UNKNOWN);
    assert(pthread_create(&thread, NULL, publish_many, NULL) == 0);
    for (i = 0; i < 100000; ++i) {
        int s = read_snapshot();
        assert(s == MEGLER_NETWORK_UNKNOWN || s == MEGLER_NETWORK_OFFLINE ||
               s == (MEGLER_NETWORK_CONNECTED | MEGLER_NETWORK_TRANSPORT_WIFI | MEGLER_NETWORK_INTERNET));
    }
    assert(pthread_join(thread, NULL) == 0);
    return 0;
}
