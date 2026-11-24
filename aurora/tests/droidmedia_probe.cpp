extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
void av_acl_droidmedia_test_snapshot(int out[8]);
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

struct Packet { std::vector<uint8_t> data; int64_t pts; uint32_t flags; };
struct Stream {
    uint32_t width, height;
    std::string mime;
    std::vector<uint8_t> config;
    std::vector<Packet> packets;
};

static uint32_t u32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

static bool readStream(const char *path, Stream &s)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (b.size() < 60 || std::memcmp(b.data(), "MGMEDIA1", 8)) return false;
    const char *mime = reinterpret_cast<const char *>(b.data() + 8);
    const char *end = static_cast<const char *>(std::memchr(mime, 0, 32));
    if (!end) return false;
    s.mime.assign(mime, end);
    s.width = u32(b.data() + 40);
    s.height = u32(b.data() + 44);
    uint32_t count = u32(b.data() + 56);
    size_t pos = 60;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos > b.size() || b.size() - pos < 16) return false;
        uint32_t flags = u32(b.data() + pos), size = u32(b.data() + pos + 4);
        uint64_t bits = u32(b.data() + pos + 8) | uint64_t(u32(b.data() + pos + 12)) << 32;
        int64_t pts;
        std::memcpy(&pts, &bits, sizeof(pts));
        pos += 16;
        if (size > b.size() - pos) return false;
        if (flags & 2) s.config.insert(s.config.end(), b.begin() + pos, b.begin() + pos + size);
        else s.packets.push_back(Packet{std::vector<uint8_t>(b.begin() + pos, b.begin() + pos + size), pts, flags});
        pos += size;
    }
    return pos == b.size() && !s.packets.empty() && s.width && s.height &&
           s.width <= 8192 && s.height <= 8192;
}

static bool receive(AVCodecContext *ctx, AVFrame *frame, std::vector<int64_t> &pts, bool &eof)
{
    for (;;) {
        int r = avcodec_receive_frame(ctx, frame);
        if (r == AVERROR(EAGAIN)) return true;
        if (r == AVERROR_EOF) { eof = true; return true; }
        if (r < 0) { std::fprintf(stderr, "receive failed: %d\n", r); return false; }
        if (frame->format != AV_PIX_FMT_YUV420P) {
            std::fprintf(stderr, "unsupported pixel format: %d\n", frame->format);
            return false;
        }
        pts.push_back(frame->pts);
        av_frame_unref(frame);
    }
}

static bool run(const char *name, const Stream &s, bool flush)
{
    const AVCodec *codec = avcodec_find_decoder_by_name(name);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = ctx && packet && frame, eof = false;
    std::vector<int64_t> got, expected;
    if (ok) {
        ctx->width = int(s.width);
        ctx->height = int(s.height);
        ctx->pkt_timebase = AVRational{1, 1000000};
    }
    if (ok) {
        int r = avcodec_open2(ctx, codec, nullptr);
        if (r < 0) { std::fprintf(stderr, "hardware decoder unavailable: %d\n", r); ok = false; }
    }
    for (int pass = flush ? 0 : 1; ok && pass < 2; ++pass) {
        size_t limit = pass == 0 ? s.packets.size() / 2 : s.packets.size();
        for (size_t i = 0; ok && i < limit; ++i) {
            const Packet &p = s.packets[i];
            size_t configSize = i == 0 ? s.config.size() : 0;
            if (configSize > 1048576 || p.data.size() > 1048576 - configSize ||
                av_new_packet(packet, int(configSize + p.data.size())) < 0) { ok = false; break; }
            if (configSize) std::memcpy(packet->data, s.config.data(), configSize);
            std::memcpy(packet->data + configSize, p.data.data(), p.data.size());
            packet->pts = p.pts;
            packet->dts = AV_NOPTS_VALUE;
            if (p.flags & 1) packet->flags |= AV_PKT_FLAG_KEY;
            int r = avcodec_send_packet(ctx, packet);
            av_packet_unref(packet);
            if (r < 0) { std::fprintf(stderr, "send failed: %d\n", r); ok = false; break; }
            ok = receive(ctx, frame, got, eof) && !eof;
            if (pass == 1) expected.push_back(p.pts);
        }
        if (pass == 0 && ok) {
            avcodec_flush_buffers(ctx);
            got.clear();
        }
    }
    if (ok) {
        int r = avcodec_send_packet(ctx, nullptr);
        if (r < 0) { std::fprintf(stderr, "drain failed: %d\n", r); ok = false; }
        if (ok) ok = receive(ctx, frame, got, eof) && eof;
        std::sort(expected.begin(), expected.end());
        if (got != expected) {
            std::fprintf(stderr, "frame PTS/count mismatch: %zu got, %zu expected\n", got.size(), expected.size());
            ok = false;
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (std::strcmp(argv[1], "h264") && std::strcmp(argv[1], "hevc"))) {
        std::fprintf(stderr, "usage: droidmedia-probe h264|hevc stream.mgmedia\n");
        return 2;
    }
    Stream stream;
    if (!readStream(argv[2], stream)) { std::fprintf(stderr, "invalid MGMEDIA1 stream\n"); return 1; }
    if (stream.mime != (std::strcmp(argv[1], "h264") ? "video/hevc" : "video/avc")) {
        std::fprintf(stderr, "stream MIME does not match the requested decoder\n");
        return 1;
    }
    const char *name = std::strcmp(argv[1], "h264") ? "hevc_droidmedia" : "h264_droidmedia";
    if (!run(name, stream, false) || !run(name, stream, true)) return 1;
    int before[8], after[8];
    av_acl_droidmedia_test_snapshot(before);
    usleep(500000);
    av_acl_droidmedia_test_snapshot(after);
    std::fprintf(stderr, "DROIDTEST callbacks=%d finished=%d active=%d late=%d eos=%d errors=%d stops=%d\n",
                 after[0], after[1], after[2], after[3], after[4], after[5], after[6]);
    if (after[2] || after[3] || after[5] || after[6] < 2 || after[0] != before[0]) return 1;
    std::puts("droidmedia probe: OK");
    return 0;
}
