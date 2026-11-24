#ifndef SHARED_MICROG_WORKER_H
#define SHARED_MICROG_WORKER_H

#include "../core/include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

int shared_microg_worker_main(int argc, char **argv, const megler_gfx_host *graphics);

#ifdef __cplusplus
}
#endif

#endif
