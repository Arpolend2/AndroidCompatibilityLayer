#include "codec_audio.h"
#include "codec_mpp_abi.h"

#include <sys/ioctl.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {

struct DmaBufSync { uint64_t flags; };
const unsigned long dmaBufSync = _IOW('b', 0, DmaBufSync);
const uint64_t dmaReadStart = 1;
const uint64_t dmaReadEnd = 1 | 4;

bool syncBuffer(int fd, uint64_t flags)
{
    DmaBufSync sync{flags};
    int result;
    do { result = ioctl(fd, dmaBufSync, &sync); } while (result < 0 && errno == EINTR);
    return result == 0;
}

struct Mpp {
    void *library = nullptr;
    int (*create)(void **, AclMppApi **) = nullptr;
    int (*init)(void *, int, int) = nullptr;
    int (*destroy)(void *) = nullptr;
    int (*check)(int, int) = nullptr;
    int (*packetInit)(void **, void *, size_t) = nullptr;
    int (*packetDeinit)(void **) = nullptr;
    void (*packetPts)(void *, int64_t) = nullptr;
    int (*packetEos)(void *) = nullptr;
    int (*frameDeinit)(void **) = nullptr;
    uint32_t (*width)(void *) = nullptr;
    uint32_t (*height)(void *) = nullptr;
    uint32_t (*hstride)(void *) = nullptr;
    uint32_t (*vstride)(void *) = nullptr;
    uint32_t (*offsetX)(void *) = nullptr;
    uint32_t (*offsetY)(void *) = nullptr;
    uint32_t (*infoChange)(void *) = nullptr;
    uint32_t (*eos)(void *) = nullptr;
    uint32_t (*discard)(void *) = nullptr;
    uint32_t (*errinfo)(void *) = nullptr;
    int64_t (*pts)(void *) = nullptr;
    int (*format)(void *) = nullptr;
    int (*colorRange)(void *) = nullptr;
    int (*colorSpace)(void *) = nullptr;
    void *(*buffer)(void *) = nullptr;
    void *(*bufferPtr)(void *, const char *) = nullptr;
    int (*bufferFd)(void *, const char *) = nullptr;
    size_t (*bufferSize)(void *, const char *) = nullptr;
    bool ready = false;

    Mpp()
    {
        library = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            std::fprintf(stderr, "ACL codec: rockchip mpp library unavailable\n");
            return;
        }
#define LOAD(field, symbol) field = reinterpret_cast<decltype(field)>(dlsym(library, symbol))
        LOAD(create, "mpp_create"); LOAD(init, "mpp_init"); LOAD(destroy, "mpp_destroy");
        LOAD(check, "mpp_check_support_format");
        LOAD(packetInit, "mpp_packet_init"); LOAD(packetDeinit, "mpp_packet_deinit");
        LOAD(packetPts, "mpp_packet_set_pts"); LOAD(packetEos, "mpp_packet_set_eos");
        LOAD(frameDeinit, "mpp_frame_deinit");
        LOAD(width, "mpp_frame_get_width"); LOAD(height, "mpp_frame_get_height");
        LOAD(hstride, "mpp_frame_get_hor_stride"); LOAD(vstride, "mpp_frame_get_ver_stride");
        LOAD(offsetX, "mpp_frame_get_offset_x"); LOAD(offsetY, "mpp_frame_get_offset_y");
        LOAD(infoChange, "mpp_frame_get_info_change"); LOAD(eos, "mpp_frame_get_eos");
        LOAD(discard, "mpp_frame_get_discard"); LOAD(errinfo, "mpp_frame_get_errinfo");
        LOAD(pts, "mpp_frame_get_pts"); LOAD(format, "mpp_frame_get_fmt");
        LOAD(colorRange, "mpp_frame_get_color_range"); LOAD(colorSpace, "mpp_frame_get_colorspace");
        LOAD(buffer, "mpp_frame_get_buffer");
        LOAD(bufferPtr, "mpp_buffer_get_ptr_with_caller");
        LOAD(bufferFd, "mpp_buffer_get_fd_with_caller");
        LOAD(bufferSize, "mpp_buffer_get_size_with_caller");
#undef LOAD
        ready = create && init && destroy && packetInit && packetDeinit && packetPts &&
                packetEos && frameDeinit && width && height && hstride && vstride && offsetX &&
                offsetY && infoChange && eos && discard && errinfo && pts && format &&
                buffer && bufferPtr && bufferFd && bufferSize;
        if (!ready) std::fprintf(stderr, "ACL codec: rockchip mpp ABI unavailable\n");
    }
};

Mpp &mpp()
{
    static Mpp *api = new Mpp;
    return *api;
}

struct Picture {
    std::vector<uint8_t> pixels;
    megler_codec_picture value{};
    size_t chromaOffset = 0;
};

struct Decoder {
    Mpp &api;
    void *ctx = nullptr;
    AclMppApi *mpi = nullptr;
    std::vector<uint8_t> config;
    std::deque<std::unique_ptr<Picture>> ready;
    size_t queuedBytes = 0;
    bool first = true;
    bool failed = false;
    bool gotEos = false;
    bool receivedInput = false;
    size_t pictures = 0;

    explicit Decoder(Mpp &functions) : api(functions) {}
    ~Decoder() { if (ctx) api.destroy(ctx); }
};

bool fail(Decoder &d, const char *reason)
{
    std::fprintf(stderr, "ACL codec: rockchip mpp %s\n", reason);
    d.failed = true;
    return false;
}

int coding(int kind)
{
    return kind == MEGLER_CODEC_H264 ? ACL_MPP_AVC :
           kind == MEGLER_CODEC_H265 ? ACL_MPP_HEVC : 0;
}

bool MEGLER_CB supports(void *, int kind)
{
    const int type = coding(kind);
    Mpp &a = mpp();
    if (!type || !a.ready) return false;
    if (a.check && a.check(ACL_MPP_DEC, type) != 0) {
        std::fprintf(stderr, "ACL codec: rockchip mpp capability rejected\n");
        return false;
    }
    return true;
}

void *MEGLER_CB open(void *, int kind, const megler_codec_format *format)
{
    if (!format || format->width < 0 || format->height < 0 ||
        format->width > 8192 || format->height > 8192 ||
        static_cast<int64_t>(format->width) * format->height > 33554432 ||
        format->config_size > 1048576 || (format->config_size && !format->config) ||
        !supports(nullptr, kind)) return nullptr;
    std::unique_ptr<Decoder> d(new Decoder(mpp()));
    if (format->config_size) d->config.assign(format->config, format->config + format->config_size);
    if (d->api.create(&d->ctx, &d->mpi) != 0 || !d->ctx || !d->mpi ||
        d->mpi->size < offsetof(AclMppApi, control) + sizeof(d->mpi->control) ||
        !d->mpi->decode_put_packet || !d->mpi->decode_get_frame || !d->mpi->reset || !d->mpi->control ||
        d->api.init(d->ctx, ACL_MPP_DEC, coding(kind)) != 0) {
        fail(*d, "initialization failed");
        return nullptr;
    }
    int blocking = -1;
    int64_t timeout = 10;
    if (d->mpi->control(d->ctx, ACL_MPP_OUTPUT_BLOCK, &blocking) != 0 ||
        d->mpi->control(d->ctx, ACL_MPP_OUTPUT_TIMEOUT, &timeout) != 0) {
        fail(*d, "output timeout setup failed");
        return nullptr;
    }
    std::fprintf(stderr, "ACL codec: rockchip mpp decoder initialized\n");
    return d.release();
}

bool copyFrame(Decoder &d, void *frame)
{
    Mpp &a = d.api;
    const uint32_t w = a.width(frame), h = a.height(frame);
    const uint32_t hs = a.hstride(frame), vs = a.vstride(frame);
    const uint32_t x = a.offsetX(frame), y = a.offsetY(frame);
    const size_t uvWidth = (static_cast<size_t>(w) + 1) & ~size_t(1);
    void *buffer = a.buffer(frame);
    if (a.format(frame) != 0 || !buffer || !w || !h || w > 8192 || h > 8192 ||
        static_cast<uint64_t>(w) * h > 33554432 || hs > 16384 || vs > 16384 || (x & 1) || (y & 1) ||
        static_cast<uint64_t>(x) + uvWidth > hs || static_cast<uint64_t>(y) + h > vs ||
        a.pts(frame) == std::numeric_limits<int64_t>::min())
        return fail(d, "output frame rejected");
    const size_t offset = static_cast<size_t>(hs) * vs;
    const size_t uvHeight = (static_cast<size_t>(h) + 1) / 2;
    const uint64_t end = static_cast<uint64_t>(offset) +
        static_cast<uint64_t>(y / 2 + uvHeight - 1) * hs + x + uvWidth;
    const size_t bufferBytes = a.bufferSize(buffer, "acl_codec_mpp");
    const size_t bytes = static_cast<size_t>(w) * h + uvWidth * uvHeight;
    if (end > bufferBytes || bytes > 64 * 1024 * 1024 ||
        d.queuedBytes > 64 * 1024 * 1024 - bytes || d.ready.size() >= 64)
        return fail(d, "output frame rejected");
    const uint8_t *base = static_cast<const uint8_t *>(a.bufferPtr(buffer, "acl_codec_mpp"));
    const int fd = a.bufferFd(buffer, "acl_codec_mpp");
    std::unique_ptr<Picture> picture(new Picture);
    picture->pixels.resize(bytes);
    picture->chromaOffset = static_cast<size_t>(w) * h;
    if (!base || fd < 0 || !syncBuffer(fd, dmaReadStart))
        return fail(d, "buffer synchronization failed");
    for (size_t row = 0; row < h; ++row)
        std::memcpy(picture->pixels.data() + row * w, base + (row + y) * hs + x, w);
    for (size_t row = 0; row < uvHeight; ++row)
        std::memcpy(picture->pixels.data() + picture->chromaOffset + row * uvWidth,
                    base + offset + (row + y / 2) * hs + x, uvWidth);
    if (!syncBuffer(fd, dmaReadEnd))
        return fail(d, "buffer synchronization failed");
    picture->value.format = MEGLER_CODEC_NV12;
    picture->value.width = static_cast<int>(w);
    picture->value.height = static_cast<int>(h);
    picture->value.stride = static_cast<int>(w);
    picture->value.chroma_stride = static_cast<int>(uvWidth);
    const int space = a.colorSpace ? a.colorSpace(frame) : 2;
    picture->value.matrix = space == 1 || (space == 2 && h >= 720) ? MEGLER_CODEC_BT709 : MEGLER_CODEC_BT601;
    picture->value.full_range = a.colorRange && a.colorRange(frame) == 2;
    picture->value.time_us = a.pts(frame);
    d.queuedBytes += bytes;
    d.ready.push_back(std::move(picture));
    ++d.pictures;
    return true;
}

bool collect(Decoder &d)
{
    for (;;) {
        void *frame = nullptr;
        const int rc = d.mpi->decode_get_frame(d.ctx, &frame);
        if (rc && rc != ACL_MPP_TIMEOUT) {
            if (frame) d.api.frameDeinit(&frame);
            return fail(d, "dequeue failed");
        }
        if (!frame) return true;
        const bool info = d.api.infoChange(frame);
        const bool eos = d.api.eos(frame);
        bool ok = true;
        if (info) {
            if (d.mpi->control(d.ctx, ACL_MPP_INFO_READY, nullptr) != 0)
                ok = fail(d, "info change failed");
        } else if (d.api.errinfo(frame)) {
            ok = fail(d, "output frame error");
        } else if (!d.api.discard(frame) && d.api.buffer(frame)) {
            ok = copyFrame(d, frame);
        } else if (!eos && !d.api.discard(frame)) {
            ok = fail(d, "output frame rejected");
        }
        if (d.api.frameDeinit(&frame) != 0) ok = fail(d, "frame release failed");
        if (!ok) return false;
        if (eos) { d.gotEos = true; return true; }
    }
}

bool send(Decoder &d, const uint8_t *data, size_t size, int64_t pts, bool eos)
{
    void *packet = nullptr;
    if (d.api.packetInit(&packet, const_cast<uint8_t *>(data), size) != 0 || !packet)
        return fail(d, "packet allocation failed");
    d.api.packetPts(packet, pts);
    bool ok = !eos || d.api.packetEos(packet) == 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ok) {
        const int rc = d.mpi->decode_put_packet(d.ctx, packet);
        if (!rc) break;
        if (rc != ACL_MPP_BUFFER_FULL || std::chrono::steady_clock::now() >= deadline || !collect(d)) {
            ok = false;
            break;
        }
    }
    if (d.api.packetDeinit(&packet) != 0) ok = false;
    return ok || fail(d, "queue failed");
}

bool MEGLER_CB push(void *, void *handle, const uint8_t *data, size_t size, int64_t time)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || d->failed || !data || !size || size > 1048576 ||
        time == std::numeric_limits<int64_t>::min()) return false;
    if (d->gotEos) d->gotEos = false;
    if (d->first && !d->config.empty() && !send(*d, d->config.data(), d->config.size(), time, false))
        return false;
    d->first = false;
    if (!send(*d, data, size, time, false)) return false;
    d->receivedInput = true;
    return collect(*d);
}

bool drain(Decoder &d)
{
    if (d.failed || !send(d, nullptr, 0, 0, true)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!d.gotEos) {
        if (!collect(d)) return false;
        if (std::chrono::steady_clock::now() >= deadline) return fail(d, "drain timeout");
    }
    if (d.receivedInput && !d.pictures) return fail(d, "drain produced no pictures");
    if (d.mpi->reset(d.ctx) != 0) return fail(d, "reset failed");
    d.first = true;
    d.receivedInput = false;
    d.pictures = 0;
    return true;
}

void MEGLER_CB drainCallback(void *, void *handle)
{
    if (handle) (void)drain(*static_cast<Decoder *>(handle));
}

void *MEGLER_CB pull(void *, void *handle, megler_codec_picture *out)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d || !out || d->failed) return nullptr;
    if (d->ready.empty() && !d->gotEos && !collect(*d)) return nullptr;
    if (d->ready.empty()) return nullptr;
    std::unique_ptr<Picture> p = std::move(d->ready.front());
    d->ready.pop_front();
    d->queuedBytes -= p->pixels.size();
    p->value.data = p->pixels.data();
    p->value.chroma = p->pixels.data() + p->chromaOffset;
    *out = p->value;
    return p.release();
}

void MEGLER_CB release(void *, void *picture) { delete static_cast<Picture *>(picture); }

void MEGLER_CB flush(void *, void *handle)
{
    Decoder *d = static_cast<Decoder *>(handle);
    if (!d) return;
    d->ready.clear();
    d->queuedBytes = 0;
    d->first = true;
    d->gotEos = false;
    d->receivedInput = false;
    d->pictures = 0;
    d->failed = false;
    if (d->mpi->reset(d->ctx) != 0) fail(*d, "reset failed");
}

void MEGLER_CB close(void *, void *handle) { delete static_cast<Decoder *>(handle); }

}

megler_codec_host auroraMppCodecCallbacks()
{
    megler_codec_host host = {};
    host.supports = supports;
    host.open = open;
    host.push = push;
    host.drain = drainCallback;
    host.pull_picture = pull;
    host.release_picture = release;
    host.flush = flush;
    host.close = close;
    return host;
}

bool auroraMppCodecDrainChecked(void *decoder)
{
    return decoder && drain(*static_cast<Decoder *>(decoder));
}

bool auroraMppCodecFailed(void *decoder)
{
    return !decoder || static_cast<Decoder *>(decoder)->failed;
}
