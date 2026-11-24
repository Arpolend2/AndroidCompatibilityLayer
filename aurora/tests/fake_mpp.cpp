#include "../codec_mpp_abi.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace {
struct Packet { int64_t pts = 0; bool eos = false; };
struct Frame { int type; int64_t pts; };
struct Context { std::deque<Frame> frames; };
int mode;
int freed;
int infoReady;
int resets;
int typeSeen;
int putCalls;
std::vector<uint8_t> pixels(48);

int put(void *context, void *packet)
{
    if (mode == 1) return -1;
    if (mode == 7 && putCalls++ == 0) return ACL_MPP_BUFFER_FULL;
    Context *ctx = static_cast<Context *>(context);
    Packet *p = static_cast<Packet *>(packet);
    if (p->eos) ctx->frames.push_back(Frame{2, p->pts});
    else {
        ctx->frames.push_back(Frame{1, p->pts});
        ctx->frames.push_back(Frame{mode == 2 ? 3 : mode == 3 ? 4 : 0, p->pts});
    }
    return 0;
}
int get(void *context, void **out)
{
    Context *ctx = static_cast<Context *>(context);
    *out = nullptr;
    if (ctx->frames.empty()) return ACL_MPP_TIMEOUT;
    *out = new Frame(ctx->frames.front());
    ctx->frames.pop_front();
    return 0;
}
int reset(void *context)
{
    ++resets;
    if (mode == 8) return -1;
    static_cast<Context *>(context)->frames.clear();
    return 0;
}
int control(void *, int cmd, void *)
{
    if (cmd == ACL_MPP_INFO_READY) {
        ++infoReady;
        if (mode == 4) return -1;
    }
    return 0;
}
AclMppApi api = {sizeof(AclMppApi), 1, nullptr, put, get, nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, reset, control};
}

extern "C" {
void fake_mpp_mode(int value) { mode = value; freed = infoReady = resets = typeSeen = putCalls = 0; }
int fake_mpp_freed() { return freed; }
int fake_mpp_info_ready() { return infoReady; }
int fake_mpp_resets() { return resets; }
int fake_mpp_coding() { return typeSeen; }
int mpp_create(void **ctx, AclMppApi **out) { *ctx = new Context; *out = &api; return 0; }
int mpp_init(void *, int, int type) { typeSeen = type; return mode == 5 ? -1 : 0; }
int mpp_destroy(void *ctx) { delete static_cast<Context *>(ctx); return 0; }
#ifndef FAKE_MPP_OMIT_OPTIONAL
int mpp_check_support_format(int, int type) { return mode != 11 && (type == ACL_MPP_AVC || type == ACL_MPP_HEVC) ? 0 : -1; }
#endif
int mpp_packet_init(void **out, void *, size_t) { *out = new Packet; return 0; }
int mpp_packet_deinit(void **packet) { delete static_cast<Packet *>(*packet); *packet = nullptr; return 0; }
void mpp_packet_set_pts(void *packet, int64_t pts) { static_cast<Packet *>(packet)->pts = pts; }
int mpp_packet_set_eos(void *packet) { static_cast<Packet *>(packet)->eos = true; return 0; }
int mpp_frame_deinit(void **frame) { ++freed; delete static_cast<Frame *>(*frame); *frame = nullptr; return 0; }
uint32_t mpp_frame_get_width(void *) { return 4; }
uint32_t mpp_frame_get_height(void *) { return 3; }
uint32_t mpp_frame_get_hor_stride(void *) { return 8; }
uint32_t mpp_frame_get_ver_stride(void *) { return 4; }
uint32_t mpp_frame_get_offset_x(void *) { return 0; }
uint32_t mpp_frame_get_offset_y(void *) { return 0; }
uint32_t mpp_frame_get_info_change(void *f) { return static_cast<Frame *>(f)->type == 1; }
uint32_t mpp_frame_get_eos(void *f) { return static_cast<Frame *>(f)->type == 2; }
uint32_t mpp_frame_get_errinfo(void *f) { return static_cast<Frame *>(f)->type == 3; }
uint32_t mpp_frame_get_discard(void *f) { return static_cast<Frame *>(f)->type == 4; }
int64_t mpp_frame_get_pts(void *f) { return static_cast<Frame *>(f)->pts; }
int mpp_frame_get_fmt(void *) { return mode == 6 ? 1 : 0; }
#ifndef FAKE_MPP_OMIT_OPTIONAL
int mpp_frame_get_color_range(void *) { return 1; }
int mpp_frame_get_colorspace(void *) { return 1; }
#endif
void *mpp_frame_get_buffer(void *f) { return static_cast<Frame *>(f)->type == 0 ? pixels.data() : nullptr; }
void *mpp_buffer_get_ptr_with_caller(void *p, const char *)
{
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i);
    return p;
}
int mpp_buffer_get_fd_with_caller(void *, const char *) { return 123; }
size_t mpp_buffer_get_size_with_caller(void *, const char *) { return mode == 9 ? 35 : pixels.size(); }
}
