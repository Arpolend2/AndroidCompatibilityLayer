#include "codec_audio.h"

#include "audio_mp3_codec.h"

#include <cstdio>
#include <memory>
#include <new>
#include <vector>

namespace {

struct RoutedDecoder {
    struct Packet {
        std::vector<uint8_t> data;
        int64_t time;
    };
    const megler_codec_host *host;
    void *decoder;
    int kind;
    int index;
    megler_codec_format format;
    std::vector<uint8_t> config;
    std::vector<Packet> replay;
    size_t replayBytes = 0;
    bool hardware;
    bool emitted = false;

    RoutedDecoder(const megler_codec_host *callbacks, void *instance, int codecKind,
                  const megler_codec_format &codecFormat, int hostIndex, bool isHardware)
        : host(callbacks), decoder(instance), kind(codecKind), index(hostIndex), format(codecFormat), hardware(isHardware)
    {
        if (format.config_size)
            config.assign(format.config, format.config + format.config_size);
        format.config = config.empty() ? nullptr : config.data();
    }
};

struct RoutedPicture {
    const megler_codec_host *host;
    void *picture;
};

const megler_codec_host &ffmpeg8Host()
{
    static const megler_codec_host host = auroraAudioCodec8Callbacks();
    return host;
}

const megler_codec_host &droidMediaHost()
{
    static const megler_codec_host host = auroraDroidMediaCodecCallbacks();
    return host;
}

const megler_codec_host &mppHost()
{
    static const megler_codec_host host = auroraMppCodecCallbacks();
    return host;
}

bool isHardware(const megler_codec_host *host)
{
    return host == &mppHost() || host == &droidMediaHost();
}

#ifdef ACL_WITH_FFMPEG7_BACKEND
const megler_codec_host &ffmpeg7Host()
{
    static const megler_codec_host host = auroraAudioCodec7Callbacks();
    return host;
}
#endif

const megler_codec_host &mp3Host()
{
    static const megler_codec_host host = auroraMp3CodecCallbacks();
    return host;
}

const megler_codec_host *hostFor(int kind, int index)
{
    if (kind == MEGLER_CODEC_MP3) return index == 0 ? &mp3Host() : nullptr;
    if (kind == MEGLER_CODEC_AAC || kind == MEGLER_CODEC_H264 || kind == MEGLER_CODEC_H265) {
        if (kind != MEGLER_CODEC_AAC) {
            if (index == 0) return &mppHost();
            if (index == 1) return &droidMediaHost();
            index -= 2;
        }
        if (index == 0) return &ffmpeg8Host();
#ifdef ACL_WITH_FFMPEG7_BACKEND
        if (index == 1) return &ffmpeg7Host();
#endif
    }
    return nullptr;
}

bool MEGLER_CB supports(void *, int kind)
{
    for (int i = 0; const megler_codec_host *host = hostFor(kind, i); ++i)
        if (host->supports && host->supports(host->user, kind)) return true;
    return false;
}

void *MEGLER_CB open(void *, int kind, const megler_codec_format *format)
{
    if (!format || (format->config_size && !format->config)) return nullptr;
    for (int i = 0; const megler_codec_host *host = hostFor(kind, i); ++i) {
        if (!host->supports || !host->supports(host->user, kind) || !host->open) {
            if (host == &droidMediaHost())
                std::fprintf(stderr, "ACL codec: droidmedia decoder or FFmpeg 8 unavailable\n");
            continue;
        }
        void *decoder = host->open(host->user, kind, format);
        if (!decoder) continue;
        std::unique_ptr<RoutedDecoder> routed(new (std::nothrow)
            RoutedDecoder(host, decoder, kind, *format, i, isHardware(host)));
        if (!routed) {
            if (host->close) host->close(host->user, decoder);
            return nullptr;
        }
        if ((kind == MEGLER_CODEC_H264 || kind == MEGLER_CODEC_H265) && !routed->hardware)
            std::fprintf(stderr, "ACL codec: video software decoder selected\n");
        return routed.release();
    }
    return nullptr;
}

RoutedDecoder *routed(void *decoder) { return static_cast<RoutedDecoder *>(decoder); }

bool fallback(RoutedDecoder &d, bool softwareOnly = false)
{
    if (!d.hardware || d.emitted) return false;
    if (d.host->close) d.host->close(d.host->user, d.decoder);
    d.decoder = nullptr;
    for (int i = d.index + 1; const megler_codec_host *host = hostFor(d.kind, i); ++i) {
        if (softwareOnly && isHardware(host)) continue;
        if (!host->supports || !host->supports(host->user, d.kind) || !host->open) continue;
        d.decoder = host->open(host->user, d.kind, &d.format);
        if (!d.decoder) continue;
        bool ok = true;
        for (const RoutedDecoder::Packet &packet : d.replay) {
            if (!host->push || !host->push(host->user, d.decoder, packet.data.data(),
                                           packet.data.size(), packet.time)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            if (host->close) host->close(host->user, d.decoder);
            d.decoder = nullptr;
            continue;
        }
        d.host = host;
        d.index = i;
        d.hardware = isHardware(host);
        if (!d.hardware) {
            d.replay.clear();
            d.replayBytes = 0;
            std::fprintf(stderr, "ACL codec: video software fallback active\n");
        } else {
            std::fprintf(stderr, "ACL codec: droidmedia hardware fallback active\n");
        }
        return true;
    }
    return false;
}

bool MEGLER_CB push(void *, void *decoder, const uint8_t *data, size_t size, int64_t time)
{
    RoutedDecoder *d = routed(decoder);
    if (!d || !d->decoder || !d->host->push || !data || !size) return false;
    if (d->hardware && !d->emitted) {
        if (d->replay.size() >= 64 || size > 8 * 1024 * 1024 - d->replayBytes) {
            std::fprintf(stderr, "ACL codec: hardware replay limit reached\n");
            if (!fallback(*d, true)) return false;
            return d->host->push(d->host->user, d->decoder, data, size, time);
        }
        d->replay.push_back(RoutedDecoder::Packet{std::vector<uint8_t>(data, data + size), time});
        d->replayBytes += size;
    }
    if (d->host->push(d->host->user, d->decoder, data, size, time)) return true;
    return d->hardware && !d->emitted && fallback(*d);
}

bool drainChecked(RoutedDecoder &d)
{
    if (!d.decoder) return false;
    bool ok;
    if (d.host == &mppHost())
        ok = auroraMppCodecDrainChecked(d.decoder);
    else if (d.host == &droidMediaHost() || d.host == &ffmpeg8Host())
        ok = auroraAudioCodec8DrainChecked(d.decoder);
#ifdef ACL_WITH_FFMPEG7_BACKEND
    else if (d.host == &ffmpeg7Host())
        ok = auroraAudioCodec7DrainChecked(d.decoder);
#endif
    else {
        if (d.host->drain) d.host->drain(d.host->user, d.decoder);
        ok = true;
    }
    return ok || (d.hardware && !d.emitted && fallback(d) && drainChecked(d));
}

void MEGLER_CB drain(void *, void *decoder)
{
    RoutedDecoder *d = routed(decoder);
    if (d) (void)drainChecked(*d);
}

void *MEGLER_CB pullPicture(void *, void *decoder, megler_codec_picture *picture)
{
    RoutedDecoder *d = routed(decoder);
    if (!d || !d->decoder || !d->host->pull_picture) return nullptr;
    void *p = d->host->pull_picture(d->host->user, d->decoder, picture);
    if (!p && d->host == &mppHost() && auroraMppCodecFailed(d->decoder) &&
        d->hardware && !d->emitted && fallback(*d))
        p = d->host->pull_picture ? d->host->pull_picture(d->host->user, d->decoder, picture) : nullptr;
    if (!p) return nullptr;
    RoutedPicture *result = new (std::nothrow) RoutedPicture{d->host, p};
    if (!result) {
        d->host->release_picture(d->host->user, p);
        return nullptr;
    }
    if (d->host == &mppHost() && !d->emitted)
        std::fprintf(stderr, "ACL codec: rockchip mpp first picture\n");
    if (d->hardware && !d->emitted)
        std::fprintf(stderr, "ACL codec: first hardware picture\n");
    d->emitted = true;
    d->replay.clear();
    d->replayBytes = 0;
    return result;
}

void MEGLER_CB releasePicture(void *, void *picture)
{
    std::unique_ptr<RoutedPicture> p(static_cast<RoutedPicture *>(picture));
    if (p && p->host->release_picture) p->host->release_picture(p->host->user, p->picture);
}

bool MEGLER_CB pullSound(void *, void *decoder, megler_codec_sound *sound)
{
    RoutedDecoder *d = routed(decoder);
    return d && d->decoder && d->host->pull_sound && d->host->pull_sound(d->host->user, d->decoder, sound);
}

void MEGLER_CB flush(void *, void *decoder)
{
    RoutedDecoder *d = routed(decoder);
    if (d && d->decoder && d->host->flush) d->host->flush(d->host->user, d->decoder);
    if (d) {
        d->emitted = false;
        d->replay.clear();
        d->replayBytes = 0;
    }
}

void MEGLER_CB close(void *, void *decoder)
{
    std::unique_ptr<RoutedDecoder> d(routed(decoder));
    if (d && d->decoder && d->host->close) d->host->close(d->host->user, d->decoder);
}

bool MEGLER_CB readPicture(void *, void *picture, megler_codec_picture *out)
{
    RoutedPicture *p = static_cast<RoutedPicture *>(picture);
    return p && p->host->read_picture && p->host->read_picture(p->host->user, p->picture, out);
}

int MEGLER_CB showPicture(void *, void *picture, int texture)
{
    RoutedPicture *p = static_cast<RoutedPicture *>(picture);
    return p && p->host->show_picture ? p->host->show_picture(p->host->user, p->picture, texture) : 0;
}

}

megler_codec_host auroraMediaCodecCallbacks()
{
    megler_codec_host host = {};
    host.reorder = 0;
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

bool MEGLER_CB auroraMediaCodecDrainChecked(void *, void *decoder)
{
    RoutedDecoder *d = routed(decoder);
    return d && drainChecked(*d);
}
