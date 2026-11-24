#include "codec_audio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <QDebug>
#include <QDir>
#include <QLibrary>

#if defined(ACL_FFMPEG_8) && !defined(ACL_FFMPEG8_TEST_DIRECTORY)
#include <auroraapp.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

#if defined(ACL_FFMPEG_8)
#if LIBAVCODEC_VERSION_MAJOR != 62 || LIBAVUTIL_VERSION_MAJOR != 60
#error The FFmpeg 8 bridge requires libavcodec 62 and libavutil 60 headers.
#endif
#define ACL_AVCODEC_MAJOR 62
#define ACL_AVUTIL_MAJOR 60
#define ACL_CODEC_CALLBACKS auroraAudioCodec8Callbacks
#define ACL_CODEC_DRAIN_CHECKED auroraAudioCodec8DrainChecked
#else
#if LIBAVCODEC_VERSION_MAJOR != 61 || LIBAVUTIL_VERSION_MAJOR != 59
#error The FFmpeg 7 bridge requires libavcodec 61 and libavutil 59 headers.
#endif
#define ACL_AVCODEC_MAJOR 61
#define ACL_AVUTIL_MAJOR 59
#define ACL_CODEC_CALLBACKS auroraAudioCodec7Callbacks
#define ACL_CODEC_DRAIN_CHECKED auroraAudioCodec7DrainChecked
#endif

namespace {

#if defined(ACL_FFMPEG_8)
int droidMediaBackend;
#endif

struct AvApi {
#if defined(ACL_FFMPEG_8)
    static QString privateLibrary(const char *name)
    {
#ifdef ACL_FFMPEG8_TEST_DIRECTORY
        const QString root = QStringLiteral(ACL_FFMPEG8_TEST_DIRECTORY);
#else
        const QString root = Aurora::Application::organizationPathTo(
            QStringLiteral("appmanager/ffmpeg8")).toLocalFile();
#endif
        return root.isEmpty() ? QString() : QDir(root).filePath(QString::fromLatin1(name));
    }
    QLibrary codec{privateLibrary("libavcodec-acl.so.62")};
    QLibrary util{privateLibrary("libavutil-acl.so.60")};
#else
    QLibrary codec{QStringLiteral("avcodec"), 61};
    QLibrary util{QStringLiteral("avutil"), 59};
#endif
    decltype(&avcodec_version) version = nullptr;
    decltype(&avutil_version) utilVersion = nullptr;
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
        codec.setLoadHints(QLibrary::DeepBindHint);
        util.setLoadHints(QLibrary::DeepBindHint);
#define LOAD(lib, member, name) member = reinterpret_cast<decltype(member)>(lib.resolve(#name))
        LOAD(util, utilVersion, avutil_version);
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
        ready = version && (version() >> 16) == ACL_AVCODEC_MAJOR && utilVersion &&
                (utilVersion() >> 16) == ACL_AVUTIL_MAJOR && find && alloc && open && freeContext &&
                send && receive && flush && packetAlloc && packetFree && newPacket &&
                packetUnref && frameAlloc && frameFree && frameUnref && mallocz;
        if (!ready) qWarning() << "AndroidCompatibilityLayer audio codec: FFmpeg" << ACL_AVCODEC_MAJOR
                               << "libraries or required symbols unavailable";
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
    bool hardware = false;
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

bool MEGLER_CB supports(void *user, int kind)
{
    AvApi &a = api();
#if defined(ACL_FFMPEG_8)
    if (user == &droidMediaBackend) {
        const char *name = kind == MEGLER_CODEC_H264 ? "h264_droidmedia" :
                           kind == MEGLER_CODEC_H265 ? "hevc_droidmedia" : nullptr;
        return name && a.ready && a.find(name);
    }
#else
    (void)user;
#endif
    const char *name = kind == MEGLER_CODEC_H264 ? "h264" :
                       kind == MEGLER_CODEC_H265 ? "hevc" :
                       kind == MEGLER_CODEC_AAC ? "aac" :
                       kind == MEGLER_CODEC_MP3 ? "mp3" : nullptr;
    return name && a.ready && a.find(name);
}

void *MEGLER_CB openCodec(void *user, int kind, const megler_codec_format *format)
{
    const bool video = kind == MEGLER_CODEC_H264 || kind == MEGLER_CODEC_H265;
    const bool validAudio = (kind == MEGLER_CODEC_AAC || kind == MEGLER_CODEC_MP3) && format &&
                            format->sample_rate > 0 && format->sample_rate <= 192000 &&
                            format->channels >= 1 && format->channels <= 2;
    const bool validVideo = video && format && format->width >= 0 && format->height >= 0 &&
                            format->width <= 8192 && format->height <= 8192 &&
                            (!format->width || !format->height ||
                             static_cast<int64_t>(format->width) * format->height <= 33554432);
    if (!format || !supports(user, kind) || (!validAudio && !validVideo) ||
        format->config_size > 1048576 || (format->config_size && !format->config)) {
        qWarning() << "AndroidCompatibilityLayer audio codec: unsupported format or decoder";
        return nullptr;
    }
    std::unique_ptr<Decoder> d(new Decoder(api()));
    d->kind = kind;
#if defined(ACL_FFMPEG_8)
    d->hardware = user == &droidMediaBackend;
#endif
    if (kind == MEGLER_CODEC_AAC) {
        if (!format->config || format->config_size < 2 || format->config_size > 1024) {
            qWarning() << "AndroidCompatibilityLayer audio codec: unsupported AAC configuration size";
            return nullptr;
        }
    }
    const char *name = kind == MEGLER_CODEC_H264 ? "h264" :
                       kind == MEGLER_CODEC_H265 ? "hevc" :
                       kind == MEGLER_CODEC_AAC ? "aac" : "mp3";
#if defined(ACL_FFMPEG_8)
    if (user == &droidMediaBackend)
        name = kind == MEGLER_CODEC_H264 ? "h264_droidmedia" : "hevc_droidmedia";
#endif
    const AVCodec *codec = d->av.find(name);
    d->context = d->av.alloc(codec);
    d->packet = d->av.packetAlloc();
    d->frame = d->av.frameAlloc();
    if (!d->context || !d->packet || !d->frame) {
        qWarning() << "AndroidCompatibilityLayer audio codec: allocation failed";
        return nullptr;
    }
    if (video) {
        d->context->pkt_timebase = AVRational{1, 1000000};
        d->context->width = format->width;
        d->context->height = format->height;
    }
    if (format->config_size) {
        d->context->extradata = static_cast<uint8_t *>(d->av.mallocz(format->config_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!d->context->extradata) {
            qWarning() << "AndroidCompatibilityLayer audio codec: configuration allocation failed";
            return nullptr;
        }
        std::memcpy(d->context->extradata, format->config, format->config_size);
        d->context->extradata_size = static_cast<int>(format->config_size);
    }
    const int openResult = d->av.open(d->context, codec, nullptr);
    if (openResult < 0) {
#if defined(ACL_FFMPEG_8)
        if (user == &droidMediaBackend)
            std::fprintf(stderr, "ACL codec: droidmedia decoder initialization failed (%d)\n", openResult);
        else
#endif
        if (kind == MEGLER_CODEC_AAC)
            std::fprintf(stderr, "ACL codec: AAC FFmpeg %d initialization failed (%d)\n",
                         ACL_AVCODEC_MAJOR, openResult);
        else
            std::fprintf(stderr, "ACL codec: video FFmpeg %d initialization failed (%d)\n",
                         ACL_AVCODEC_MAJOR, openResult);
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
    const bool tenBit = format == AV_PIX_FMT_YUV420P10LE;
    const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || tenBit;
    const int sampleBytes = tenBit ? 2 : 1;
    const bool nv12 = format == AV_PIX_FMT_NV12;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        static_cast<int64_t>(width) * height > 33554432 ||
        (!planar && !nv12) || !frame->data[0] || !frame->data[1] ||
        (planar && !frame->data[2]) || frame->pts == AV_NOPTS_VALUE ||
        std::abs(static_cast<int64_t>(frame->linesize[0])) < width * sampleBytes) {
        qWarning() << "AndroidCompatibilityLayer video codec: unsupported decoded picture";
        return false;
    }
    const int chromaWidth = (width + 1) / 2;
    if ((planar && (std::abs(static_cast<int64_t>(frame->linesize[1])) < chromaWidth * sampleBytes ||
                    std::abs(static_cast<int64_t>(frame->linesize[2])) < chromaWidth * sampleBytes)) ||
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
            const auto sample = [tenBit](const uint8_t *p, int index) {
                if (!tenBit) return static_cast<int>(p[index]);
                return static_cast<int>((p[index * 2] | (p[index * 2 + 1] << 8)) >> 2);
            };
            const int u = (planar ? sample(uPlane, chromaX) : uvPlane[chromaX * 2]) - 128;
            const int v = (planar ? sample(vPlane, chromaX) : uvPlane[chromaX * 2 + 1]) - 128;
            const int rawY = sample(luma, x);
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

bool collect(Decoder &d, bool *eof = nullptr)
{
    if (eof) *eof = false;
    for (;;) {
        const int result = d.av.receive(d.context, d.frame);
        if (result == AVERROR(EAGAIN)) return true;
        if (result == AVERROR_EOF) {
            if (eof) *eof = true;
            return true;
        }
        if (result < 0) {
            if (d.hardware)
                std::fprintf(stderr, "ACL codec: droidmedia dequeue failed\n");
            else
                qWarning() << "AndroidCompatibilityLayer audio codec: receive failed:" << result;
            return false;
        }
        AVFrame *f = d.frame;
        if (d.kind == MEGLER_CODEC_H264 || d.kind == MEGLER_CODEC_H265) {
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

bool startsWithAnnexB(const uint8_t *data, std::size_t size)
{
    std::size_t i = 0;
    while (i < size && data[i] == 0) ++i;
    return i >= 2 && i < size && data[i] == 1;
}

bool isLengthPrefixed(const uint8_t *data, std::size_t size, std::size_t lengthBytes)
{
    std::size_t at = 0;
    while (at < size) {
        if (size - at < lengthBytes) return false;
        std::size_t length = 0;
        for (std::size_t i = 0; i < lengthBytes; ++i)
            length = (length << 8) | data[at++];
        if (!length || length > size - at) return false;
        at += length;
    }
    return at == size;
}

bool startsWithIdr(int kind, const uint8_t *data, std::size_t size)
{
    if ((kind != MEGLER_CODEC_H264 && kind != MEGLER_CODEC_H265) ||
        !startsWithAnnexB(data, size) || isLengthPrefixed(data, size, 1) ||
        isLengthPrefixed(data, size, 2) || isLengthPrefixed(data, size, 4)) return false;

    std::size_t start = 0;
    while (start + 3 < size) {
        while (start + 3 < size &&
               (data[start] || data[start + 1] || data[start + 2] != 1)) ++start;
        if (start + 3 >= size) break;
        const std::size_t nal = start + 3;
        std::size_t end = nal;
        while (end + 3 <= size &&
               (data[end] || data[end + 1] || data[end + 2] != 1)) ++end;
        if (end + 3 > size) end = size;
        while (end > nal && !data[end - 1]) --end;

        if (data[nal] & 0x80) return false;
        if (kind == MEGLER_CODEC_H264) {
            const int type = data[nal] & 0x1f;
            if (type >= 1 && type <= 5)
                return end > nal + 1 && type == 5;
        } else {
            if (end <= nal + 1 || !(data[nal + 1] & 7)) return false;
            const int type = (data[nal] >> 1) & 0x3f;
            if (type < 32)
                return end > nal + 2 && (type == 19 || type == 20);
        }
        start = end;
    }
    return false;
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
    d.packet->dts = AV_NOPTS_VALUE;
    if (startsWithIdr(d.kind, data, size))
        d.packet->flags |= AV_PKT_FLAG_KEY;
    const int result = d.av.send(d.context, d.packet);
    d.av.packetUnref(d.packet);
    if (result < 0) {
        if (d.hardware)
            std::fprintf(stderr, "ACL codec: droidmedia queue failed\n");
        else
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
    if (d->kind == MEGLER_CODEC_AAC || d->kind == MEGLER_CODEC_H264 || d->kind == MEGLER_CODEC_H265) {
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

bool drainChecked(void *handle)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || d->failed) return false;
    const int result = d->av.send(d->context, nullptr);
    bool eof = false;
    if ((result < 0 && result != AVERROR_EOF) || !collect(*d, &eof) || !eof) {
        if (d->hardware)
            std::fprintf(stderr, "ACL codec: droidmedia drain failed\n");
        else
            qWarning() << "AndroidCompatibilityLayer audio codec: drain failed:" << result;
        d->failed = true;
    }
    d->av.flush(d->context);
    return !d->failed;
}

void MEGLER_CB drain(void *, void *handle)
{
    (void)drainChecked(handle);
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

megler_codec_host ACL_CODEC_CALLBACKS()
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

bool ACL_CODEC_DRAIN_CHECKED(void *decoder)
{
    return drainChecked(decoder);
}

#if defined(ACL_FFMPEG_8)
megler_codec_host auroraDroidMediaCodecCallbacks()
{
    megler_codec_host host = ACL_CODEC_CALLBACKS();
    host.user = &droidMediaBackend;
    return host;
}
#endif
