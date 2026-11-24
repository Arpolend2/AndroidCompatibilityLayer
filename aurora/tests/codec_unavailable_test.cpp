#include "../codec_audio.h"

#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const megler_codec_host host = auroraMediaCodecCallbacks();
    return host.supports(host.user, MEGLER_CODEC_MP3) &&
           !host.supports(host.user, MEGLER_CODEC_H264) &&
           !host.supports(host.user, MEGLER_CODEC_H265) &&
           !host.supports(host.user, MEGLER_CODEC_AAC) ? 0 : 1;
}
