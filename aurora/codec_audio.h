#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H

#include "../core/include/megler.h"

// The returned callbacks have process lifetime and need no user object.
megler_codec_host auroraAudioCodecCallbacks();

// Route AAC/H.264 to FFmpeg when available; keep MP3 on the bundled minimp3 host.
megler_codec_host auroraMediaCodecCallbacks();

#endif
