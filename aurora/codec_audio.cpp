#include "codec_audio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <QDebug>
#include <QLibrary>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

#if LIBAVCODEC_VERSION_MAJOR != 61 || LIBAVUTIL_VERSION_MAJOR != 59
#error This bridge requires FFmpeg 7 libavcodec 61 and libavutil 59 headers.
#endif

namespace {

struct AvApi {
    QLibrary codec{QStringLiteral("avcodec"), 61};
    QLibrary util{QStringLiteral("avutil"), 59};
    decltype(&avcodec_version) version = nullptr;
    decltype(&avcodec_find_decoder_by_name) find = nullptr;
    decltype(&avcodec_alloc_context3) alloc = nullptr;
    decltype(&avcodec_open2) open = nullptr;
    decltype(&avcodec_free_context) freeContext = nullptr;
    decltype(&avcodec_send_packet) send = nullptr;
    decltype(&avcodec_receive_frame) receive = nullptr;
    decltype(&avcodec_flush_buffers) flush = nullptr;
    decltype(&av_packet_alloc) packetAlloc = nullptr;
    decltype(&av_packet_free) packetFree = nullptr;
    decltype(&av_new_packet) newPacket = nullptr;
    decltype(&av_packet_unref) packetUnref = nullptr;
    decltype(&av_frame_alloc) frameAlloc = nullptr;
    decltype(&av_frame_free) frameFree = nullptr;
    decltype(&av_frame_unref) frameUnref = nullptr;
    decltype(&av_mallocz) mallocz = nullptr;
    bool ready = false;

    AvApi()
    {
#define LOAD(lib, member, name) member = reinterpret_cast<decltype(member)>(lib.resolve(#name))
        LOAD(codec, version, avcodec_version);
        LOAD(codec, find, avcodec_find_decoder_by_name);
        LOAD(codec, alloc, avcodec_alloc_context3);
        LOAD(codec, open, avcodec_open2);
        LOAD(codec, freeContext, avcodec_free_context);
        LOAD(codec, send, avcodec_send_packet);
        LOAD(codec, receive, avcodec_receive_frame);
        LOAD(codec, flush, avcodec_flush_buffers);
        LOAD(codec, packetAlloc, av_packet_alloc);
        LOAD(codec, packetFree, av_packet_free);
        LOAD(codec, newPacket, av_new_packet);
        LOAD(codec, packetUnref, av_packet_unref);
        LOAD(util, frameAlloc, av_frame_alloc);
        LOAD(util, frameFree, av_frame_free);
        LOAD(util, frameUnref, av_frame_unref);
        LOAD(util, mallocz, av_mallocz);
#undef LOAD
        ready = version && (version() >> 16) == 61 && find && alloc && open && freeContext &&
                send && receive && flush && packetAlloc && packetFree && newPacket &&
                packetUnref && frameAlloc && frameFree && frameUnref && mallocz;
        if (!ready) qWarning() << "AndroidCompatibilityLayer audio codec: FFmpeg 7 libraries or required symbols unavailable";
    }
};

AvApi &api()
{
    static AvApi *instance = new AvApi;
    return *instance;
}

struct Sound {
    std::vector<int16_t> samples;
    int rate = 0;
    int channels = 0;
    int64_t time = 0;
};

struct Picture {
    std::vector<uint8_t> rgba;
    megler_codec_picture value{};
};

struct Decoder {
    AvApi &av;
    AVCodecContext *context = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int kind = 0;
    std::deque<Sound> ready;
    std::deque<Picture> readyPictures;
    Sound current;
    std::size_t queuedSamples = 0;
    std::size_t queuedPictureBytes = 0;
    bool failed = false;

    explicit Decoder(AvApi &functions) : av(functions) {}
    ~Decoder()
    {
        if (packet) av.packetFree(&packet);
        if (frame) av.frameFree(&frame);
        if (context) av.freeContext(&context);
    }
};

bool MEGLER_CB supports(void *, int kind)
{
    AvApi &a = api();
    const char *name = kind == MEGLER_CODEC_H264 ? "h264" :
                       kind == MEGLER_CODEC_AAC ? "aac" :
                       kind == MEGLER_CODEC_MP3 ? "mp3" : nullptr;
    return name && a.ready && a.find(name);
}

void *MEGLER_CB openCodec(void *, int kind, const megler_codec_format *format)
{
    const bool video = kind == MEGLER_CODEC_H264;
    const bool validAudio = (kind == MEGLER_CODEC_AAC || kind == MEGLER_CODEC_MP3) && format &&
                            format->sample_rate > 0 && format->sample_rate <= 192000 &&
                            format->channels >= 1 && format->channels <= 2;
    const bool validVideo = video && format && format->width >= 0 && format->height >= 0 &&
                            format->width <= 8192 && format->height <= 8192 &&
                            (!format->width || !format->height ||
                             static_cast<int64_t>(format->width) * format->height <= 33554432);
    if (!format || !supports(nullptr, kind) || (!validAudio && !validVideo) ||
        format->config_size > 1048576 || (format->config_size && !format->config)) {
        qWarning() << "AndroidCompatibilityLayer audio codec: unsupported format or decoder";
        return nullptr;
    }
    std::unique_ptr<Decoder> d(new Decoder(api()));
    d->kind = kind;
    if (kind == MEGLER_CODEC_AAC) {
        if (!format->config || format->config_size < 2 || format->config_size > 1024) {
            qWarning() << "AndroidCompatibilityLayer audio codec: unsupported AAC configuration size";
            return nullptr;
        }
    }
    const char *name = video ? "h264" : kind == MEGLER_CODEC_AAC ? "aac" : "mp3";
    const AVCodec *codec = d->av.find(name);
    d->context = d->av.alloc(codec);
    d->packet = d->av.packetAlloc();
    d->frame = d->av.frameAlloc();
    if (!d->context || !d->packet || !d->frame) {
        qWarning() << "AndroidCompatibilityLayer audio codec: allocation failed";
        return nullptr;
    }
    if (video) d->context->pkt_timebase = AVRational{1, 1000000};
    if (format->config_size) {
        d->context->extradata = static_cast<uint8_t *>(d->av.mallocz(format->config_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!d->context->extradata) {
            qWarning() << "AndroidCompatibilityLayer audio codec: configuration allocation failed";
            return nullptr;
        }
        std::memcpy(d->context->extradata, format->config, format->config_size);
        d->context->extradata_size = static_cast<int>(format->config_size);
    }
    if (d->av.open(d->context, codec, nullptr) < 0) {
        qWarning() << "AndroidCompatibilityLayer audio codec: decoder initialization failed";
        return nullptr;
    }
    return d.release();
}

int16_t sample16(double value)
{
    if (!std::isfinite(value)) return 0;
    value = std::max(-1.0, std::min(value, 32767.0 / 32768.0));
    return static_cast<int16_t>(std::lrint(value * 32768.0));
}

static uint8_t clampByte(int value)
{
    return static_cast<uint8_t>(std::max(0, std::min(value, 255)));
}

bool collectPicture(Decoder &d, AVFrame *frame)
{
    const int width = frame->width, height = frame->height;
    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
    const bool nv12 = format == AV_PIX_FMT_NV12;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        static_cast<int64_t>(width) * height > 33554432 ||
        (!planar && !nv12) || !frame->data[0] || !frame->data[1] ||
        (planar && !frame->data[2]) || frame->pts == AV_NOPTS_VALUE ||
        std::abs(static_cast<int64_t>(frame->linesize[0])) < width) {
        qWarning() << "AndroidCompatibilityLayer video codec: unsupported decoded picture";
        return false;
    }
    const int chromaWidth = (width + 1) / 2;
    if ((planar && (std::abs(static_cast<int64_t>(frame->linesize[1])) < chromaWidth ||
                    std::abs(static_cast<int64_t>(frame->linesize[2])) < chromaWidth)) ||
        (nv12 && std::abs(static_cast<int64_t>(frame->linesize[1])) < chromaWidth * 2)) {
        qWarning() << "AndroidCompatibilityLayer video codec: invalid decoded picture stride";
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const std::size_t bytes = pixels * 4;
    if (bytes > 64 * 1024 * 1024 || d.queuedPictureBytes > 64 * 1024 * 1024 - bytes) {
        qWarning() << "AndroidCompatibilityLayer video codec: decoded picture queue full";
        return false;
    }

    const bool bt709 = frame->colorspace == AVCOL_SPC_BT709 ||
                       (frame->colorspace == AVCOL_SPC_UNSPECIFIED && height >= 720);
    const bool fullRange = format == AV_PIX_FMT_YUVJ420P || frame->color_range == AVCOL_RANGE_JPEG;
    Picture picture;
    picture.rgba.resize(bytes);
    for (int y = 0; y < height; ++y) {
        const uint8_t *luma = frame->data[0] + static_cast<int64_t>(y) * frame->linesize[0];
        const uint8_t *uPlane = planar
            ? frame->data[1] + static_cast<int64_t>(y / 2) * frame->linesize[1] : nullptr;
        const uint8_t *vPlane = planar
            ? frame->data[2] + static_cast<int64_t>(y / 2) * frame->linesize[2] : nullptr;
        const uint8_t *uvPlane = nv12
            ? frame->data[1] + static_cast<int64_t>(y / 2) * frame->linesize[1] : nullptr;
        for (int x = 0; x < width; ++x) {
            const int chromaX = x / 2;
            const int u = (planar ? uPlane[chromaX] : uvPlane[chromaX * 2]) - 128;
            const int v = (planar ? vPlane[chromaX] : uvPlane[chromaX * 2 + 1]) - 128;
            const int rawY = luma[x];
            const int yy = fullRange ? rawY : std::max(0, rawY - 16);
            int r, g, b;
            if (fullRange) {
                r = yy + (bt709 ? (403 * v + 128) / 256 : (359 * v + 128) / 256);
                g = yy - (bt709 ? (48 * u + 120 * v + 128) / 256
                                : (88 * u + 183 * v + 128) / 256);
                b = yy + (bt709 ? (475 * u + 128) / 256 : (454 * u + 128) / 256);
            } else {
                r = (298 * yy + (bt709 ? 459 : 409) * v + 128) / 256;
                g = (298 * yy - (bt709 ? 55 : 100) * u - (bt709 ? 136 : 208) * v + 128) / 256;
                b = (298 * yy + (bt709 ? 541 : 516) * u + 128) / 256;
            }
            uint8_t *pixel = picture.rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = clampByte(r);
            pixel[1] = clampByte(g);
            pixel[2] = clampByte(b);
            pixel[3] = 255;
        }
    }
    picture.value.format = MEGLER_CODEC_RGBA;
    picture.value.width = width;
    picture.value.height = height;
    picture.value.stride = width * 4;
    picture.value.matrix = bt709 ? MEGLER_CODEC_BT709 : MEGLER_CODEC_BT601;
    picture.value.full_range = true;
    picture.value.time_us = frame->pts;
    d.queuedPictureBytes += bytes;
    d.readyPictures.push_back(std::move(picture));
    return true;
}

bool collect(Decoder &d)
{
    for (;;) {
        const int result = d.av.receive(d.context, d.frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) {
            qWarning() << "AndroidCompatibilityLayer audio codec: receive failed:" << result;
            return false;
        }
        AVFrame *f = d.frame;
        if (d.kind == MEGLER_CODEC_H264) {
            const bool ok = collectPicture(d, f);
            d.av.frameUnref(f);
            if (!ok) return false;
            continue;
        }
        const int channels = f->ch_layout.nb_channels;
        const int count = f->nb_samples;
        const AVSampleFormat format = static_cast<AVSampleFormat>(f->format);
        if (channels < 1 || channels > 2 || count <= 0 || count > 32768 ||
            f->sample_rate <= 0 || f->sample_rate > 192000 || !f->extended_data ||
            !f->extended_data[0] || (channels == 2 &&
            (format == AV_SAMPLE_FMT_S16P || format == AV_SAMPLE_FMT_FLTP) && !f->extended_data[1]) ||
            f->pts == AV_NOPTS_VALUE ||
            (format != AV_SAMPLE_FMT_S16 && format != AV_SAMPLE_FMT_S16P &&
             format != AV_SAMPLE_FMT_FLT && format != AV_SAMPLE_FMT_FLTP) ||
            d.queuedSamples + static_cast<std::size_t>(count) * channels > 2097152) {
            qWarning() << "AndroidCompatibilityLayer audio codec: unsupported PCM format or output queue full";
            d.av.frameUnref(f);
            return false;
        }
        Sound sound;
        sound.rate = f->sample_rate;
        sound.channels = channels;
        sound.time = f->pts == AV_NOPTS_VALUE ? 0 : f->pts;
        sound.samples.resize(static_cast<std::size_t>(count) * channels);
        const bool planar = format == AV_SAMPLE_FMT_S16P || format == AV_SAMPLE_FMT_FLTP;
        const bool floating = format == AV_SAMPLE_FMT_FLT || format == AV_SAMPLE_FMT_FLTP;
        for (int i = 0; i < count; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                const int index = planar ? i : i * channels + ch;
                const uint8_t *plane = f->extended_data[planar ? ch : 0];
                sound.samples[static_cast<std::size_t>(i) * channels + ch] = floating
                    ? sample16(reinterpret_cast<const float *>(plane)[index])
                    : reinterpret_cast<const int16_t *>(plane)[index];
            }
        }
        d.queuedSamples += sound.samples.size();
        d.ready.push_back(std::move(sound));
        d.av.frameUnref(f);
    }
}

bool sendPacket(Decoder &d, const uint8_t *data, std::size_t size, int64_t time)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        d.av.newPacket(d.packet, static_cast<int>(size)) < 0) {
        qWarning() << "AndroidCompatibilityLayer audio codec: packet allocation failed";
        return false;
    }
    std::memcpy(d.packet->data, data, size);
    d.packet->pts = time;
    d.packet->dts = time;
    const int result = d.av.send(d.context, d.packet);
    d.av.packetUnref(d.packet);
    if (result < 0) {
        qWarning() << "AndroidCompatibilityLayer audio codec: packet rejected:" << result;
        return false;
    }
    return collect(d);
}

bool mp3Frame(const uint8_t *data, std::size_t size, std::size_t &length, int &rate, int &samples)
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
    samples = version == 3 ? 1152 : 576;
    const int kbps = version == 3 ? br1[bitrate] : br2[bitrate];
    length = static_cast<std::size_t>((version == 3 ? 144000 : 72000) * kbps / rate + ((data[2] >> 1) & 1));
    return length >= 4 && length <= size;
}

bool MEGLER_CB push(void *, void *handle, const uint8_t *data, size_t size, int64_t time)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || d->failed || !data || !size || size > 1048576 || time == AV_NOPTS_VALUE) {
        qWarning() << "AndroidCompatibilityLayer audio codec: invalid packet";
        return false;
    }
    bool ok = true;
    if (d->kind == MEGLER_CODEC_AAC || d->kind == MEGLER_CODEC_H264) {
        ok = sendPacket(*d, data, size, time);
    } else {
        while (size && ok) {
            std::size_t n;
            int rate, samples;
            if (!mp3Frame(data, size, n, rate, samples)) {
                qWarning() << "AndroidCompatibilityLayer audio codec: invalid MP3 frame";
                ok = false;
                break;
            }
            ok = sendPacket(*d, data, n, time);
            data += n;
            size -= n;
            const int64_t delta = static_cast<int64_t>(samples) * 1000000 / rate;
            if (time > std::numeric_limits<int64_t>::max() - delta) {
                qWarning() << "AndroidCompatibilityLayer audio codec: timestamp overflow";
                ok = false;
                break;
            }
            time += delta;
        }
    }
    if (!ok) d->failed = true;
    return ok;
}

void MEGLER_CB drain(void *, void *handle)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || d->failed) return;
    const int result = d->av.send(d->context, nullptr);
    if ((result < 0 && result != AVERROR_EOF) || !collect(*d)) {
        qWarning() << "AndroidCompatibilityLayer audio codec: drain failed:" << result;
        d->failed = true;
    }
    d->av.flush(d->context);
}

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

void *MEGLER_CB pullPicture(void *, void *handle, megler_codec_picture *out)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || !out || d->readyPictures.empty()) return nullptr;
    std::unique_ptr<Picture> picture(new Picture(std::move(d->readyPictures.front())));
    d->readyPictures.pop_front();
    d->queuedPictureBytes -= picture->rgba.size();
    picture->value.data = picture->rgba.data();
    *out = picture->value;
    return picture.release();
}

void MEGLER_CB releasePicture(void *, void *picture) { delete static_cast<Picture *>(picture); }

void MEGLER_CB flush(void *, void *handle)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d) return;
    d->av.flush(d->context);
    d->ready.clear();
    d->queuedSamples = 0;
    d->readyPictures.clear();
    d->queuedPictureBytes = 0;
    d->failed = false;
}

void MEGLER_CB closeCodec(void *, void *handle) { delete static_cast<Decoder *>(handle); }

}

megler_codec_host auroraAudioCodecCallbacks()
{
    megler_codec_host host = {};
    host.reorder = 0;
    host.supports = supports;
    host.open = openCodec;
    host.push = push;
    host.drain = drain;
    host.pull_picture = pullPicture;
    host.release_picture = releasePicture;
    host.pull_sound = pull;
    host.flush = flush;
    host.close = closeCodec;
    return host;
}
