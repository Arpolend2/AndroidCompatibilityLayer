#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_CODEC_AUDIO_H

#include "../core/include/megler.h"

// The returned callbacks have process lifetime and need no user object.
megler_codec_host auroraAudioCodec7Callbacks();
megler_codec_host auroraAudioCodec8Callbacks();
megler_codec_host auroraDroidMediaCodecCallbacks();
megler_codec_host auroraMppCodecCallbacks();
bool auroraMppCodecDrainChecked(void *decoder);
bool auroraMppCodecFailed(void *decoder);
bool auroraAudioCodec8DrainChecked(void *decoder);
bool auroraAudioCodec7DrainChecked(void *decoder);
bool MEGLER_CB auroraMediaCodecDrainChecked(void *user, void *decoder);

// Route video through hardware backends before FFmpeg; keep MP3 on minimp3.
megler_codec_host auroraMediaCodecCallbacks();
struct mg_demux_host;
const mg_demux_host *auroraDemuxCallbacks();

#endif
