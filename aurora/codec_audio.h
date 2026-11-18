#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H

#include "../core/include/megler.h"

// The returned callbacks have process lifetime and need no user object.
megler_codec_host auroraAudioCodecCallbacks();

#endif
