/* libmediandk.so and the SurfaceTexture a decoder draws to: a game's films. See mg_media.c. */
#ifndef MG_MEDIA_H
#define MG_MEDIA_H

#include "../bionic/mg_bionic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libmediandk.so's functions and data, for the linker. */
const mg_sym *mg_media_table(void);
/* The Java half (SurfaceTexture, a Surface made of one); after a driver's own table. */
void mg_media_install(void);
/* For the tests: the decoder to use instead of the running app's shell's (NULL: that one again). */
struct megler_media_host;
void mg_media_set_host(const struct megler_media_host *host);

typedef struct mg_demux_track {
    const char *mime;
    i32 width, height, sample_rate, channels;
    i64 duration_us;
    const u8 *config;
    size_t config_size;
} mg_demux_track;

typedef struct mg_demux_sample {
    const u8 *data;
    size_t size;
    int track;
    i64 time_us;
    u32 flags;
} mg_demux_sample;

typedef struct mg_demux_host {
    void *(*open)(const char *path);
    void (*close)(void *demux);
    int (*track_count)(void *demux);
    bool (*track)(void *demux, int index, mg_demux_track *track);
    int (*read)(void *demux, mg_demux_sample *sample);
    bool (*seek)(void *demux, i64 time_us, int mode);
} mg_demux_host;

/* Set before a run; the host and its callbacks must remain valid while extractors exist. */
void mg_media_set_demux_host(const mg_demux_host *host);

#ifdef __cplusplus
}
#endif

#endif
