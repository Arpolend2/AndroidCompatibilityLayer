#include "../codec_audio.h"

#include <cstdio>
#include <dlfcn.h>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct FakeDecoder { bool hardware; };
static int hardwareMarker;
static bool failDroid;
static int droidOpens, softwareOpens;
static std::vector<int64_t> droidPackets, softwarePackets;

static bool MEGLER_CB supports(void *, int kind) { return kind == MEGLER_CODEC_H264; }
static void *MEGLER_CB open(void *user, int, const megler_codec_format *)
{
    const bool hardware = user != nullptr;
    if (hardware) ++droidOpens;
    else ++softwareOpens;
    return new FakeDecoder{hardware};
}
static bool MEGLER_CB push(void *, void *handle, const uint8_t *, size_t, int64_t pts)
{
    const bool hardware = static_cast<FakeDecoder *>(handle)->hardware;
    if (hardware && failDroid) return false;
    (hardware ? droidPackets : softwarePackets).push_back(pts);
    return true;
}
static void MEGLER_CB close(void *, void *handle) { delete static_cast<FakeDecoder *>(handle); }
static megler_codec_host callbacks(void *user)
{
    megler_codec_host host = {};
    host.user = user;
    host.supports = supports;
    host.open = open;
    host.push = push;
    host.close = close;
    return host;
}
megler_codec_host auroraAudioCodec8Callbacks() { return callbacks(nullptr); }
megler_codec_host auroraDroidMediaCodecCallbacks() { return callbacks(&hardwareMarker); }
megler_codec_host auroraMp3CodecCallbacks() { return {}; }
bool auroraAudioCodec8DrainChecked(void *) { return true; }

int main()
{
    void *lib = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_LOCAL);
    CHECK(lib);
    auto setMode = reinterpret_cast<void (*)(int)>(dlsym(lib, "fake_mpp_mode"));
    CHECK(setMode);
    megler_codec_host host = auroraMediaCodecCallbacks();
    megler_codec_format format = {};
    const uint8_t bytes[] = {0, 0, 1, 0x65};

    setMode(1);
    void *d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && !droidOpens);
    CHECK(host.push(host.user, d, bytes, sizeof(bytes), 42));
    CHECK(droidOpens == 1 && !softwareOpens && droidPackets == std::vector<int64_t>{42});
    host.close(host.user, d);

    setMode(2);
    failDroid = true;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, bytes, sizeof(bytes), 87));
    CHECK(softwareOpens == 1 && softwarePackets == std::vector<int64_t>{87});
    host.close(host.user, d);
    setMode(3);
    failDroid = false;
    droidPackets.clear();
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, bytes, sizeof(bytes), 99));
    CHECK(auroraMediaCodecDrainChecked(host.user, d));
    CHECK(droidPackets == std::vector<int64_t>{99});
    host.close(host.user, d);
    std::puts("MPP routed fallback: OK");
    return 0;
}
