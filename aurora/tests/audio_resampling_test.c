#include "../../core/src/ndk/mg_audio.c"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const i16 first[] = {0, 0, 8192, -8192};
    const i16 second[] = {16384, -16384, 24576, -24576};
    mg_voice voice = {0};
    float mixed[12] = {0};
    voice.channels = 2;
    voice.bits = 16;
    voice.frame_bytes = 4;
    voice.step = 32768;
    voice.gain = voice.gain_l = voice.gain_r = 1.0f;
    voice.queue[0].data = (const u8 *)first;
    voice.queue[0].frames = 2;
    voice.queue[1].data = (const u8 *)second;
    voice.queue[1].frames = 2;
    voice.count = 2;
    if (mix_voice(&voice, mixed, 6) != 1) return 1;
    for (int i = 0; i < 6; ++i) {
        const float expected = i * 0.125f;
        if (fabsf(mixed[2 * i] - expected) > 0.00001f ||
            fabsf(mixed[2 * i + 1] + expected) > 0.00001f) {
            fprintf(stderr, "interpolation failed at frame %d: %f %f\n",
                    i, mixed[2 * i], mixed[2 * i + 1]);
            return 1;
        }
    }
    memset(mixed, 0, sizeof(mixed));
    if (mix_voice(&voice, mixed, 2) != 1 ||
        fabsf(mixed[0] - 0.75f) > 0.00001f ||
        fabsf(mixed[2] - 0.75f) > 0.00001f) {
        fprintf(stderr, "final sample was not held at the queue boundary\n");
        return 1;
    }
    memset(&voice, 0, sizeof(voice));
    memset(mixed, 0, sizeof(mixed));
    voice.channels = 2;
    voice.bits = 16;
    voice.frame_bytes = 4;
    voice.step = (u32)(((u64)24000 << 16) / MG_AUDIO_RATE);
    voice.gain = voice.gain_l = voice.gain_r = 1.0f;
    voice.head = MAX_QUEUE - 1;
    voice.queue[MAX_QUEUE - 1].data = (const u8 *)first;
    voice.queue[MAX_QUEUE - 1].frames = 2;
    voice.queue[0].data = (const u8 *)second;
    voice.queue[0].frames = 2;
    voice.count = 2;
    mix_voice(&voice, mixed, 6);
    for (int i = 0; i < 6; ++i) {
        const float expected = (float)(i * voice.step) / 65536.0f * 0.25f;
        if (fabsf(mixed[2 * i] - expected) > 0.00001f ||
            fabsf(mixed[2 * i + 1] + expected) > 0.00001f) {
            fprintf(stderr, "24 kHz interpolation failed at frame %d\n", i);
            return 1;
        }
    }
    puts("Audio resampling OK");
    return 0;
}
