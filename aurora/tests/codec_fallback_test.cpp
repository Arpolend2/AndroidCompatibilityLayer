#include "../codec_audio.h"

#include <cstdio>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct Packet { std::vector<uint8_t> bytes; int64_t time; };
struct Fake { bool hardware; bool ready; bool mpp; };
static std::vector<Packet> softwarePackets;
static std::vector<uint8_t> softwareConfig;
static int hardwareOpenFail, hardwarePushFail, hardwareDrainFail, hardwareReady, softwareOpens;
static int hardwareClosed;
static int mppMarker, droidMarker, mppEnabled, mppOpenFail, mppPushFail, mppDrainFail;
static int mppPullFail, mppClosed, mppOpens, droidOpens;
static std::vector<Packet> droidPackets;

static bool MEGLER_CB supports(void *user, int kind) {
    return kind == MEGLER_CODEC_H264 && (user != &mppMarker || mppEnabled);
}
static void *MEGLER_CB fakeOpen(void *user, int, const megler_codec_format *format)
{
    const bool hardware = user != nullptr;
    const bool mpp = user == &mppMarker;
    if (mpp) ++mppOpens;
    if (hardware && !mpp) ++droidOpens;
    if ((mpp && mppOpenFail) || (hardware && !mpp && hardwareOpenFail)) return nullptr;
    if (!hardware) {
        softwareOpens++;
        if (format->config_size)
            softwareConfig.assign(format->config, format->config + format->config_size);
    }
    return new Fake{hardware, false, mpp};
}
static bool MEGLER_CB fakePush(void *, void *decoder, const uint8_t *data, size_t size, int64_t time)
{
    Fake *d = static_cast<Fake *>(decoder);
    if (d->mpp ? mppPushFail : (d->hardware && hardwarePushFail)) return false;
    if (!d->hardware) softwarePackets.push_back(Packet{std::vector<uint8_t>(data, data + size), time});
    if (d->hardware && !d->mpp) droidPackets.push_back(Packet{std::vector<uint8_t>(data, data + size), time});
    if (d->hardware && hardwareReady) d->ready = true;
    return true;
}
static void *MEGLER_CB fakePicture(void *, void *decoder, megler_codec_picture *picture)
{
    Fake *d = static_cast<Fake *>(decoder);
    if (d->mpp && mppPullFail) return nullptr;
    if (!d->ready) return nullptr;
    d->ready = false;
    *picture = {};
    return new int(1);
}
static void MEGLER_CB fakeRelease(void *, void *picture) { delete static_cast<int *>(picture); }
static void MEGLER_CB fakeFlush(void *, void *decoder) { static_cast<Fake *>(decoder)->ready = false; }
static void MEGLER_CB fakeClose(void *, void *decoder)
{
    Fake *d = static_cast<Fake *>(decoder);
    if (d->hardware) hardwareClosed++;
    if (d->mpp) mppClosed++;
    delete d;
}
static megler_codec_host callbacks(void *user)
{
    megler_codec_host host = {};
    host.user = user;
    host.supports = supports;
    host.open = fakeOpen;
    host.push = fakePush;
    host.pull_picture = fakePicture;
    host.release_picture = fakeRelease;
    host.flush = fakeFlush;
    host.close = fakeClose;
    return host;
}
megler_codec_host auroraAudioCodec8Callbacks() { return callbacks(nullptr); }
megler_codec_host auroraDroidMediaCodecCallbacks() { return callbacks(&droidMarker); }
megler_codec_host auroraMppCodecCallbacks() { return callbacks(&mppMarker); }
megler_codec_host auroraMp3CodecCallbacks() { return {}; }
bool auroraMppCodecDrainChecked(void *) { return !mppDrainFail; }
bool auroraMppCodecFailed(void *) { return mppPullFail; }
bool auroraAudioCodec8DrainChecked(void *decoder)
{
    return !(static_cast<Fake *>(decoder)->hardware && hardwareDrainFail);
}
static void reset()
{
    hardwareOpenFail = hardwarePushFail = hardwareDrainFail = hardwareReady = 0;
    mppEnabled = mppOpenFail = mppPushFail = mppDrainFail = mppPullFail = 0;
    mppClosed = mppOpens = droidOpens = 0;
    softwareOpens = hardwareClosed = 0;
    softwarePackets.clear();
    droidPackets.clear();
    softwareConfig.clear();
}

int main()
{
    megler_codec_host host = auroraMediaCodecCallbacks();
    uint8_t config[] = {7, 9};
    megler_codec_format format = {};
    format.config = config;
    format.config_size = sizeof(config);
    const uint8_t first[] = {1, 2}, second[] = {3, 4};

    reset();
    hardwareOpenFail = 1;
    void *d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && softwareOpens == 1);
    host.close(host.user, d);

    reset();
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 42));
    config[0] = 99;
    hardwarePushFail = 1;
    CHECK(host.push(host.user, d, second, sizeof(second), 42));
    CHECK(hardwareClosed == 1 && softwareOpens == 1 && softwarePackets.size() == 2);
    CHECK(softwareConfig == std::vector<uint8_t>({7, 9}));
    CHECK(softwarePackets[0].bytes == std::vector<uint8_t>({1, 2}) && softwarePackets[0].time == 42);
    CHECK(softwarePackets[1].bytes == std::vector<uint8_t>({3, 4}) && softwarePackets[1].time == 42);
    host.close(host.user, d);
    config[0] = 7;

    reset();
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 1));
    hardwareDrainFail = 1;
    CHECK(auroraMediaCodecDrainChecked(host.user, d));
    CHECK(softwarePackets.size() == 1 && softwarePackets[0].time == 1);
    host.close(host.user, d);

    reset();
    hardwareReady = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 1));
    megler_codec_picture picture = {};
    void *p = host.pull_picture(host.user, d, &picture);
    CHECK(p);
    host.release_picture(host.user, p);
    hardwarePushFail = 1;
    CHECK(!host.push(host.user, d, second, sizeof(second), 2));
    hardwareDrainFail = 1;
    CHECK(!auroraMediaCodecDrainChecked(host.user, d) && !softwareOpens);
    host.flush(host.user, d);
    hardwarePushFail = 1;
    CHECK(host.push(host.user, d, second, sizeof(second), 3));
    CHECK(softwarePackets.size() == 1 && softwarePackets[0].time == 3);
    host.close(host.user, d);

    reset();
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d);
    for (int i = 0; i < 65; i++)
        CHECK(host.push(host.user, d, first, sizeof(first), i));
    CHECK(hardwareClosed == 1 && softwarePackets.size() == 65);
    for (int i = 0; i < 65; i++) CHECK(softwarePackets[i].time == i);
    host.close(host.user, d);

    reset();
    mppEnabled = mppOpenFail = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && mppOpens == 1 && droidOpens == 1 && !softwareOpens);
    host.close(host.user, d);

    reset();
    mppEnabled = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 10));
    mppPushFail = 1;
    CHECK(host.push(host.user, d, second, sizeof(second), 20));
    CHECK(mppClosed == 1 && droidOpens == 1 && droidPackets.size() == 2 && !softwareOpens);
    hardwarePushFail = 1;
    CHECK(host.push(host.user, d, first, sizeof(first), 30));
    CHECK(softwarePackets.size() == 3 && softwarePackets[0].time == 10 &&
          softwarePackets[1].time == 20 && softwarePackets[2].time == 30);
    host.close(host.user, d);

    reset();
    mppEnabled = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 10));
    mppPullFail = hardwarePushFail = 1;
    megler_codec_picture failedPicture = {};
    CHECK(!host.pull_picture(host.user, d, &failedPicture));
    CHECK(mppClosed == 1 && droidOpens == 1 && softwarePackets.size() == 1);
    host.close(host.user, d);

    reset();
    mppEnabled = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 10));
    mppDrainFail = 1;
    CHECK(auroraMediaCodecDrainChecked(host.user, d));
    CHECK(mppClosed == 1 && droidPackets.size() == 1 && !softwareOpens);
    host.close(host.user, d);

    reset();
    mppEnabled = hardwareReady = 1;
    d = host.open(host.user, MEGLER_CODEC_H264, &format);
    CHECK(d && host.push(host.user, d, first, sizeof(first), 10));
    megler_codec_picture mppPicture = {};
    void *mppFrame = host.pull_picture(host.user, d, &mppPicture);
    CHECK(mppFrame);
    host.release_picture(host.user, mppFrame);
    mppPushFail = 1;
    CHECK(!host.push(host.user, d, second, sizeof(second), 20));
    CHECK(!droidOpens && !softwareOpens);
    host.close(host.user, d);
    std::puts("codec hardware fallback: OK");
    return 0;
}
