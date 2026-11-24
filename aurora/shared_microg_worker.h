#ifndef SHARED_MICROG_WORKER_H
#define SHARED_MICROG_WORKER_H

#include "../core/include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

int shared_microg_worker_main(int argc, char **argv, const megler_gfx_host *graphics);
bool MEGLER_CB shared_microg_gpu_present_pixels(void *user, const void *pixels, int width, int height, int stride);
bool shared_microg_gpu_frame_owner(uint64_t *channel, uint64_t *generation);

#ifdef __cplusplus
}
#endif

#endif
