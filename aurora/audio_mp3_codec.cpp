#include "audio_mp3_codec.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "audio_minimp3.h"

#include <array>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

struct Sound {
    std::vector<int16_t> samples;
    int channels;
    int rate;
    int64_t time;
};

struct Decoder {
    mp3dec_t state{};
    std::deque<Sound> ready;
    Sound current{};
    std::size_t queuedSamples = 0;
    bool failed = false;
    Decoder() { mp3dec_init(&state); }
};

bool MEGLER_CB supports(void *, int kind) { return kind == MEGLER_CODEC_MP3; }

void *MEGLER_CB openCodec(void *, int kind, const megler_codec_format *format)
{
    if (!supports(nullptr, kind) || !format || format->sample_rate <= 0 ||
        format->sample_rate > 192000 || format->channels < 1 || format->channels > 2) {
        std::fprintf(stderr, "AndroidCompatibilityLayer MP3 codec: unsupported format\n");
        return nullptr;
    }
    return new Decoder;
}

bool frameSize(const uint8_t *data, std::size_t size, std::size_t &length, int &rate, int &frames)
{
    static const int rates[] = {44100, 48000, 32000};
    static const int br1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int br2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    if (size < 4 || data[0] != 0xff || (data[1] & 0xe0) != 0xe0) return false;
    const int version = (data[1] >> 3) & 3;
    const int layer = (data[1] >> 1) & 3;
    const int bitrate = data[2] >> 4;
    const int index = (data[2] >> 2) & 3;
    if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || index == 3) return false;
    rate = rates[index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    frames = version == 3 ? 1152 : 576;
    const int kbps = version == 3 ? br1[bitrate] : br2[bitrate];
    length = static_cast<std::size_t>((version == 3 ? 144000 : 72000) * kbps / rate + ((data[2] >> 1) & 1));
    return length >= 4 && length <= size;
}

bool MEGLER_CB push(void *, void *handle, const uint8_t *data, size_t size, int64_t time)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || d->failed || !data || !size || size > 1048576) {
        std::fprintf(stderr, "AndroidCompatibilityLayer MP3 codec: invalid packet or decoder state\n");
        return false;
    }
    while (size) {
        std::size_t length;
        int rate, frames;
        if (!frameSize(data, size, length, rate, frames)) break;
        const int64_t delta = static_cast<int64_t>(frames) * 1000000 / rate;
        if (time > std::numeric_limits<int64_t>::max() - delta) break;
        std::array<int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> samples{};
        mp3dec_frame_info_t info{};
        const int decoded = mp3dec_decode_frame(&d->state, data, static_cast<int>(length), samples.data(), &info);
        if (info.frame_offset != 0 || info.frame_bytes != static_cast<int>(length) ||
            info.layer != 3 || info.hz != rate || info.channels < 1 || info.channels > 2 ||
            decoded < 0 || decoded > frames ||
            d->queuedSamples + static_cast<std::size_t>(decoded) * info.channels > 2097152) break;
        if (decoded) {
            Sound block;
            block.channels = info.channels;
            block.rate = rate;
            block.time = time;
            const std::size_t count = static_cast<std::size_t>(decoded) * info.channels;
            block.samples.assign(samples.begin(), samples.begin() + count);
            d->queuedSamples += count;
            d->ready.push_back(std::move(block));
        }
        data += length;
        size -= length;
        time += delta;
    }
    if (!size) return true;
    d->failed = true;
    std::fprintf(stderr, "AndroidCompatibilityLayer MP3 codec: invalid frame, timestamp or output queue full\n");
    return false;
}

void MEGLER_CB drain(void *, void *) {}

bool MEGLER_CB pull(void *, void *handle, megler_codec_sound *out)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || !out || d->ready.empty()) return false;
    d->current = std::move(d->ready.front());
    d->ready.pop_front();
    d->queuedSamples -= d->current.samples.size();
    out->samples = d->current.samples.data();
    out->frames = static_cast<int>(d->current.samples.size() / d->current.channels);
    out->channels = d->current.channels;
    out->sample_rate = d->current.rate;
    out->time_us = d->current.time;
    return true;
}

void MEGLER_CB flush(void *, void *handle)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d) return;
    d->state = mp3dec_t{};
    mp3dec_init(&d->state);
    d->ready.clear();
    d->queuedSamples = 0;
    d->failed = false;
}

void MEGLER_CB closeCodec(void *, void *handle) { delete static_cast<Decoder *>(handle); }

}

megler_codec_host auroraMp3CodecCallbacks()
{
    megler_codec_host host = {};
    host.supports = supports;
    host.open = openCodec;
    host.push = push;
    host.drain = drain;
    host.pull_sound = pull;
    host.flush = flush;
    host.close = closeCodec;
    return host;
}
