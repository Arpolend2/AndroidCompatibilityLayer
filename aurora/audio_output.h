#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_AUDIO_OUTPUT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_AUDIO_OUTPUT_H

#include "../core/include/megler.h"

class AuroraAudioOutput
{
public:
    AuroraAudioOutput();
    ~AuroraAudioOutput();
    AuroraAudioOutput(const AuroraAudioOutput &) = delete;
    AuroraAudioOutput &operator=(const AuroraAudioOutput &) = delete;

    megler_audio_host callbacks();
    void shutdown();

private:
    static bool MEGLER_CB openCallback(void *user, int rate);
    static void MEGLER_CB writeCallback(void *user, const int16_t *samples, int frames);
    class Impl;
    Impl *m_impl;
};

#endif
