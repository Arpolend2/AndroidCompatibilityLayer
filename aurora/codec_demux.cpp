#include "codec_audio.h"
#include "../core/src/ndk/mg_media.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include <QDebug>
#include <QDir>
#include <QLibrary>
#ifndef ACL_FFMPEG8_TEST_DIRECTORY
#include <auroraapp.h>
#endif

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#if LIBAVFORMAT_VERSION_MAJOR != 62
#error The private demuxer requires libavformat 62.
#endif

namespace {

QString libraryPath(const char *name)
{
#ifdef ACL_FFMPEG8_TEST_DIRECTORY
    const QString root = QStringLiteral(ACL_FFMPEG8_TEST_DIRECTORY);
#else
    const QString root = Aurora::Application::organizationPathTo(
        QStringLiteral("appmanager/ffmpeg8")).toLocalFile();
#endif
    return root.isEmpty() ? QString() : QDir(root).filePath(QString::fromLatin1(name));
}

struct DemuxApi {
    QLibrary util{libraryPath("libavutil-acl.so.60")};
    QLibrary codec{libraryPath("libavcodec-acl.so.62")};
    QLibrary format{libraryPath("libavformat-acl.so.62")};
    decltype(&avformat_version) version = nullptr;
    decltype(&avformat_open_input) open = nullptr;
    decltype(&avformat_find_stream_info) findInfo = nullptr;
    decltype(&avformat_close_input) close = nullptr;
    decltype(&av_read_frame) read = nullptr;
    decltype(&avformat_seek_file) seek = nullptr;
    decltype(&av_packet_alloc) packetAlloc = nullptr;
    decltype(&av_packet_free) packetFree = nullptr;
    decltype(&av_packet_unref) packetUnref = nullptr;
    decltype(&av_rescale_q) rescale = nullptr;
    bool ready = false;

    DemuxApi()
    {
        util.setLoadHints(QLibrary::DeepBindHint);
        codec.setLoadHints(QLibrary::DeepBindHint);
        format.setLoadHints(QLibrary::DeepBindHint);
        if (!util.load() || !codec.load() || !format.load()) {
            qWarning() << "ACL demux: private FFmpeg libraries unavailable";
            return;
        }
#define LOAD(lib, member, name) member = reinterpret_cast<decltype(member)>(lib.resolve(#name))
        LOAD(format, version, avformat_version);
        LOAD(format, open, avformat_open_input);
        LOAD(format, findInfo, avformat_find_stream_info);
        LOAD(format, close, avformat_close_input);
        LOAD(format, read, av_read_frame);
        LOAD(format, seek, avformat_seek_file);
        LOAD(codec, packetAlloc, av_packet_alloc);
        LOAD(codec, packetFree, av_packet_free);
        LOAD(codec, packetUnref, av_packet_unref);
        LOAD(util, rescale, av_rescale_q);
#undef LOAD
        ready = version && (version() >> 16) == 62 && open && findInfo && close &&
                read && seek && packetAlloc && packetFree && packetUnref && rescale;
        if (!ready) qWarning() << "ACL demux: incompatible FFmpeg libraries";
    }
};

DemuxApi &api()
{
    static DemuxApi *instance = new DemuxApi;
    return *instance;
}

const char *mimeFor(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_AV1: return "video/av01";
    case AV_CODEC_ID_MPEG2VIDEO: return "video/mpeg2";
    case AV_CODEC_ID_MPEG4: return "video/mp4v-es";
    case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
    case AV_CODEC_ID_MP3: return "audio/mpeg";
    case AV_CODEC_ID_OPUS: return "audio/opus";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    case AV_CODEC_ID_FLAC: return "audio/flac";
    case AV_CODEC_ID_AC3: return "audio/ac3";
    case AV_CODEC_ID_EAC3: return "audio/eac3";
    default: return nullptr;
    }
}

struct Demux {
    DemuxApi &av;
    AVFormatContext *format = nullptr;
    AVPacket *packet = nullptr;
    std::vector<int> streams;

    explicit Demux(DemuxApi &functions) : av(functions) {}
    ~Demux()
    {
        if (packet) av.packetFree(&packet);
        if (format) av.close(&format);
    }
};

void *openDemux(const char *path)
{
    DemuxApi &av = api();
    if (!path || !av.ready) return nullptr;
    std::unique_ptr<Demux> demux(new Demux(av));
    if (av.open(&demux->format, path, nullptr, nullptr) < 0 ||
        av.findInfo(demux->format, nullptr) < 0 ||
        !(demux->packet = av.packetAlloc())) {
        qWarning() << "ACL demux: cannot open media file";
        return nullptr;
    }
    for (unsigned i = 0; i < demux->format->nb_streams; ++i)
        if (mimeFor(demux->format->streams[i]->codecpar->codec_id))
            demux->streams.push_back(static_cast<int>(i));
    if (demux->streams.empty() || demux->streams.size() > 32) {
        qWarning() << "ACL demux: no supported tracks or too many tracks";
        return nullptr;
    }
    return demux.release();
}

void closeDemux(void *handle) { delete static_cast<Demux *>(handle); }

int trackCount(void *handle)
{
    return static_cast<int>(static_cast<Demux *>(handle)->streams.size());
}

bool track(void *handle, int index, mg_demux_track *out)
{
    Demux &demux = *static_cast<Demux *>(handle);
    if (!out || index < 0 || static_cast<size_t>(index) >= demux.streams.size()) return false;
    const AVStream *stream = demux.format->streams[demux.streams[index]];
    const AVCodecParameters *p = stream->codecpar;
    *out = {};
    out->mime = mimeFor(p->codec_id);
    out->width = p->width;
    out->height = p->height;
    out->sample_rate = p->sample_rate;
    out->channels = p->ch_layout.nb_channels;
    out->duration_us = stream->duration == AV_NOPTS_VALUE ? demux.format->duration :
        demux.av.rescale(stream->duration, stream->time_base, AVRational{1, 1000000});
    out->config = p->extradata;
    out->config_size = p->extradata_size > 0 ? static_cast<size_t>(p->extradata_size) : 0;
    return out->mime != nullptr && out->config_size <= 1048576;
}

int readSample(void *handle, mg_demux_sample *out)
{
    Demux &demux = *static_cast<Demux *>(handle);
    demux.av.packetUnref(demux.packet);
    for (;;) {
        int rc = demux.av.read(demux.format, demux.packet);
        if (rc < 0) return rc == AVERROR_EOF ? 0 : -1;
        int index = -1;
        for (size_t i = 0; i < demux.streams.size(); ++i)
            if (demux.streams[i] == demux.packet->stream_index) {
                index = static_cast<int>(i);
                break;
            }
        if (index < 0) {
            demux.av.packetUnref(demux.packet);
            continue;
        }
        if (!demux.packet->data || demux.packet->size <= 0 || demux.packet->size > 1048576 ||
            demux.packet->pts == AV_NOPTS_VALUE) {
            qWarning() << "ACL demux: invalid, oversized or untimed packet";
            return -1;
        }
        *out = {};
        out->data = demux.packet->data;
        out->size = static_cast<size_t>(demux.packet->size);
        out->track = index;
        out->time_us = demux.av.rescale(demux.packet->pts,
            demux.format->streams[demux.packet->stream_index]->time_base, AVRational{1, 1000000});
        out->flags = (demux.packet->flags & AV_PKT_FLAG_KEY) ? 1u : 0u;
        return 1;
    }
}

bool seek(void *handle, i64 time, int mode)
{
    Demux &demux = *static_cast<Demux *>(handle);
    if (mode < 0 || mode > 2) return false;
    const int64_t min = mode == 1 ? time : std::numeric_limits<int64_t>::min();
    const int64_t max = mode == 0 ? time : std::numeric_limits<int64_t>::max();
    int rc = demux.av.seek(demux.format, -1, min, time, max, 0);
    if (rc < 0) qWarning() << "ACL demux: seek failed:" << rc;
    return rc >= 0;
}

} // namespace

const mg_demux_host *auroraDemuxCallbacks()
{
    static const mg_demux_host callbacks = {openDemux, closeDemux, trackCount, track, readSample, seek};
    return &callbacks;
}
