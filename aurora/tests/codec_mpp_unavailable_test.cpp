#include "../codec_audio.h"

#include <cstdio>

int main()
{
    const megler_codec_host host = auroraMppCodecCallbacks();
    if (host.supports(host.user, MEGLER_CODEC_H264) ||
        host.supports(host.user, MEGLER_CODEC_H265)) {
        std::fputs("MPP unexpectedly available\n", stderr);
        return 1;
    }
    std::puts("MPP unavailable fallback: OK");
    return 0;
}
