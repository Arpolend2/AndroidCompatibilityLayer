#include "codec_audio.h"

#include "audio_mp3_codec.h"

#include <memory>
#include <new>

namespace {

struct RoutedDecoder {
    const megler_codec_host *host;
    void *decoder;

    RoutedDecoder(const megler_codec_host *callbacks, void *instance)
        : host(callbacks), decoder(instance) {}
};

const megler_codec_host &ffmpegHost()
{
    static const megler_codec_host host = auroraAudioCodecCallbacks();
    return host;
}

const megler_codec_host &mp3Host()
{
    static const megler_codec_host host = auroraMp3CodecCallbacks();
    return host;
}

const megler_codec_host *hostFor(int kind)
{
    if (kind == MEGLER_CODEC_MP3) return &mp3Host();
    if (kind == MEGLER_CODEC_AAC || kind == MEGLER_CODEC_H264) return &ffmpegHost();
    return nullptr;
}

bool MEGLER_CB supports(void *, int kind)
{
    const megler_codec_host *host = hostFor(kind);
    return host && host->supports && host->supports(host->user, kind);
}

void *MEGLER_CB open(void *, int kind, const megler_codec_format *format)
{
    const megler_codec_host *host = hostFor(kind);
    if (!host || !host->supports || !host->supports(host->user, kind) || !host->open) return nullptr;
    void *decoder = host->open(host->user, kind, format);
    if (!decoder) return nullptr;
    std::unique_ptr<RoutedDecoder> routed(new (std::nothrow) RoutedDecoder(host, decoder));
    if (!routed) {
        if (host->close) host->close(host->user, decoder);
        return nullptr;
    }
    return routed.release();
}

RoutedDecoder *routed(void *decoder) { return static_cast<RoutedDecoder *>(decoder); }

bool MEGLER_CB push(void *, void *decoder, const uint8_t *data, size_t size, int64_t time)
{
    RoutedDecoder *d = routed(decoder);
    return d && d->host->push && d->host->push(d->host->user, d->decoder, data, size, time);
}

void MEGLER_CB drain(void *, void *decoder)
{
    RoutedDecoder *d = routed(decoder);
    if (d && d->host->drain) d->host->drain(d->host->user, d->decoder);
}

void *MEGLER_CB pullPicture(void *, void *decoder, megler_codec_picture *picture)
{
    RoutedDecoder *d = routed(decoder);
    return d && d->host->pull_picture
        ? d->host->pull_picture(d->host->user, d->decoder, picture) : nullptr;
}

void MEGLER_CB releasePicture(void *, void *picture)
{
    const megler_codec_host &host = ffmpegHost();
    if (host.release_picture) host.release_picture(host.user, picture);
}

bool MEGLER_CB pullSound(void *, void *decoder, megler_codec_sound *sound)
{
    RoutedDecoder *d = routed(decoder);
    return d && d->host->pull_sound && d->host->pull_sound(d->host->user, d->decoder, sound);
}

void MEGLER_CB flush(void *, void *decoder)
{
    RoutedDecoder *d = routed(decoder);
    if (d && d->host->flush) d->host->flush(d->host->user, d->decoder);
}

void MEGLER_CB close(void *, void *decoder)
{
    std::unique_ptr<RoutedDecoder> d(routed(decoder));
    if (d && d->host->close) d->host->close(d->host->user, d->decoder);
}

bool MEGLER_CB readPicture(void *, void *picture, megler_codec_picture *out)
{
    const megler_codec_host &host = ffmpegHost();
    return host.read_picture && host.read_picture(host.user, picture, out);
}

int MEGLER_CB showPicture(void *, void *picture, int texture)
{
    const megler_codec_host &host = ffmpegHost();
    return host.show_picture ? host.show_picture(host.user, picture, texture) : 0;
}

}

megler_codec_host auroraMediaCodecCallbacks()
{
    megler_codec_host host = {};
    host.reorder = ffmpegHost().reorder;
    host.supports = supports;
    host.open = open;
    host.push = push;
    host.drain = drain;
    host.pull_picture = pullPicture;
    host.release_picture = releasePicture;
    host.pull_sound = pullSound;
    host.flush = flush;
    host.close = close;
    host.read_picture = readPicture;
    host.show_picture = showPicture;
    return host;
}
