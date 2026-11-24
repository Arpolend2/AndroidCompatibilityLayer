#include "../codec_audio.h"
#include "../codec_mpp_abi.h"

#include <sys/ioctl.h>
#include <cstdarg>
#include <cstdio>
#include <dlfcn.h>
#include <initializer_list>
#include <cerrno>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int syncCalls;
static bool syncFail;
extern "C" int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    const uint64_t *flags = va_arg(args, const uint64_t *);
    va_end(args);
    if (fd != 123 || request != _IOW('b', 0, uint64_t) || !flags ||
        *flags != (syncCalls % 2 ? 5u : 1u)) return -1;
    ++syncCalls;
    if (syncFail) { errno = EIO; return -1; }
    return 0;
}

int main()
{
    void *lib = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_LOCAL);
    CHECK(lib);
    auto setMode = reinterpret_cast<void (*)(int)>(dlsym(lib, "fake_mpp_mode"));
    auto freed = reinterpret_cast<int (*)()>(dlsym(lib, "fake_mpp_freed"));
    auto info = reinterpret_cast<int (*)()>(dlsym(lib, "fake_mpp_info_ready"));
    auto resets = reinterpret_cast<int (*)()>(dlsym(lib, "fake_mpp_resets"));
    auto coding = reinterpret_cast<int (*)()>(dlsym(lib, "fake_mpp_coding"));
    const bool hasColorSpace = dlsym(lib, "mpp_frame_get_colorspace") != nullptr;
    const bool hasCapabilityCheck = dlsym(lib, "mpp_check_support_format") != nullptr;
    CHECK(setMode && freed && info && resets && coding);
    megler_codec_host host = auroraMppCodecCallbacks();
    megler_codec_format format = {};
    const uint8_t input[] = {0, 0, 1, 0x65};
    CHECK(host.supports(host.user, MEGLER_CODEC_H264));
    CHECK(host.supports(host.user, MEGLER_CODEC_H265));
    CHECK(!host.supports(host.user, MEGLER_CODEC_AAC));
    setMode(11);
    CHECK(host.supports(host.user, MEGLER_CODEC_H264) == !hasCapabilityCheck);

    setMode(0);
    void *d = host.open(host.user, MEGLER_CODEC_H265, &format);
    CHECK(d && coding() == ACL_MPP_HEVC);
    CHECK(host.push(host.user, d, input, sizeof(input), 123456));
    CHECK(info() == 1 && freed() == 2 && syncCalls == 2);
    megler_codec_picture picture = {};
    void *p = host.pull_picture(host.user, d, &picture);
    CHECK(p && picture.format == MEGLER_CODEC_NV12 && picture.width == 4 && picture.height == 3);
    CHECK(picture.stride == 4 && picture.chroma_stride == 4 && picture.time_us == 123456);
    CHECK(picture.data[0] == 0 && picture.data[4] == 8 && picture.chroma[0] == 32 && picture.chroma[4] == 40);
    CHECK(picture.matrix == (hasColorSpace ? MEGLER_CODEC_BT709 : MEGLER_CODEC_BT601) && !picture.full_range);
    CHECK(auroraMppCodecDrainChecked(d) && freed() == 3 && resets() == 1);
    host.flush(host.user, d);
    CHECK(resets() == 2);
    host.close(host.user, d);
    CHECK(picture.data[4] == 8 && picture.chroma[4] == 40);
    host.release_picture(host.user, p);
    setMode(0);
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && auroraMppCodecDrainChecked(d) && freed() == 1);
    host.close(host.user, d);

    for (int mode : {1, 2, 4, 6, 9}) {
        setMode(mode);
        syncCalls = 0;
        d = host.open(host.user, MEGLER_CODEC_H264, &format);
        CHECK(d && coding() == ACL_MPP_AVC);
        CHECK(!host.push(host.user, d, input, sizeof(input), 1));
        CHECK(auroraMppCodecFailed(d));
        if (mode != 1) CHECK(freed() >= 1);
        host.close(host.user, d);
    }
    setMode(3);
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, input, sizeof(input), 1));
    CHECK(!host.pull_picture(host.user, d, &picture) && freed() == 2);
    CHECK(!auroraMppCodecDrainChecked(d) && auroraMppCodecFailed(d));
    host.close(host.user, d);

    setMode(5);
    CHECK(!host.open(host.user, MEGLER_CODEC_H264, &format));
    setMode(7);
    syncCalls = 0;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, input, sizeof(input), 2));
    p = host.pull_picture(host.user, d, &picture);
    CHECK(p && picture.time_us == 2);
    host.release_picture(host.user, p);
    host.close(host.user, d);
    setMode(8);
    syncCalls = 0;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, input, sizeof(input), 2));
    CHECK(!auroraMppCodecDrainChecked(d) && auroraMppCodecFailed(d));
    host.close(host.user, d);
    setMode(0);
    syncCalls = 0;
    syncFail = true;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && !host.push(host.user, d, input, sizeof(input), 1) && freed() == 2);
    host.close(host.user, d);
    std::puts("MPP dynamic ABI and frame handling: OK");
    return 0;
}
