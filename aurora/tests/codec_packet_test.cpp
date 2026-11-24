#include "../codec_audio.cpp"

#include <cstdio>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static int64_t actualPts;
static int64_t actualDts;
static int actualFlags;

static int capturePacket(AVCodecContext *, const AVPacket *packet)
{
    actualPts = packet->pts;
    actualDts = packet->dts;
    actualFlags = packet->flags;
    return 0;
}

static int noFrames(AVCodecContext *, AVFrame *)
{
    return AVERROR(EAGAIN);
}

static int sendEnd(AVCodecContext *, const AVPacket *) { return 0; }
static int endOfStream(AVCodecContext *, AVFrame *) { return AVERROR_EOF; }
static void noFlush(AVCodecContext *) {}

int main()
{
    AvApi &functions = api();
    CHECK(functions.ready);
    const megler_codec_host hardware = auroraDroidMediaCodecCallbacks();
    CHECK(hardware.supports(hardware.user, MEGLER_CODEC_H264));
    CHECK(hardware.supports(hardware.user, MEGLER_CODEC_H265));
    CHECK(!hardware.supports(hardware.user, MEGLER_CODEC_AAC));
    functions.send = capturePacket;
    functions.receive = noFrames;
    Decoder decoder(functions);
    decoder.packet = functions.packetAlloc();
    decoder.frame = functions.frameAlloc();
    CHECK(decoder.packet && decoder.frame);

    const uint8_t h264Idr[] = {0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x65, 1};
    const uint8_t h264Delta[] = {0, 0, 1, 0x41, 5};
    const uint8_t h264DeltaBeforeIdr[] = {0, 0, 1, 0x41, 5, 0, 0, 1, 0x65, 1};
    const uint8_t h264LengthPrefixed[] = {0, 0, 0, 2, 0x65, 1};
    const uint8_t h264TruncatedIdr[] = {0, 0, 1, 0x65};
    const uint8_t hevcIdr[] = {0, 0, 0, 1, 0x26, 1, 5};
    const uint8_t hevcConfigThenIdr[] = {0, 0, 1, 0x40, 1, 5, 0, 0, 0, 1, 0x28, 1, 5};
    const uint8_t hevcDelta[] = {0, 0, 1, 0x02, 1, 5};
    const uint8_t hevcCra[] = {0, 0, 1, 0x2a, 1, 5};
    const uint8_t hevcInvalidHeader[] = {0, 0, 1, 0x26, 0, 5};
    const uint8_t incomplete[] = {0, 0, 1};

    decoder.kind = MEGLER_CODEC_H264;
    CHECK(sendPacket(decoder, h264Idr, sizeof(h264Idr), 123456));
    CHECK(actualPts == 123456 && actualDts == AV_NOPTS_VALUE && (actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, h264Delta, sizeof(h264Delta), 123456));
    CHECK(actualPts == 123456 && actualDts == AV_NOPTS_VALUE && !(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, incomplete, sizeof(incomplete), -10));
    CHECK(actualPts == -10 && actualDts == AV_NOPTS_VALUE && !(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, h264DeltaBeforeIdr, sizeof(h264DeltaBeforeIdr), 123456));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, h264LengthPrefixed, sizeof(h264LengthPrefixed), 0));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    std::vector<uint8_t> ambiguousLengthPrefixed(4 + 0x165, 0x55);
    ambiguousLengthPrefixed[0] = 0;
    ambiguousLengthPrefixed[1] = 0;
    ambiguousLengthPrefixed[2] = 1;
    ambiguousLengthPrefixed[3] = 0x65;
    CHECK(sendPacket(decoder, ambiguousLengthPrefixed.data(), ambiguousLengthPrefixed.size(), 0));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, h264TruncatedIdr, sizeof(h264TruncatedIdr), 0));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    decoder.kind = MEGLER_CODEC_H265;
    CHECK(sendPacket(decoder, hevcIdr, sizeof(hevcIdr), 42));
    CHECK(actualPts == 42 && actualDts == AV_NOPTS_VALUE && (actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, hevcConfigThenIdr, sizeof(hevcConfigThenIdr), 42));
    CHECK(actualPts == 42 && (actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, hevcDelta, sizeof(hevcDelta), 41));
    CHECK(actualPts == 41 && actualDts == AV_NOPTS_VALUE && !(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, hevcCra, sizeof(hevcCra), 40));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    CHECK(sendPacket(decoder, hevcInvalidHeader, sizeof(hevcInvalidHeader), 39));
    CHECK(!(actualFlags & AV_PKT_FLAG_KEY));
    decoder.kind = MEGLER_CODEC_AAC;
    CHECK(sendPacket(decoder, h264Idr, sizeof(h264Idr), 2));
    CHECK(actualPts == 2 && actualDts == AV_NOPTS_VALUE && !(actualFlags & AV_PKT_FLAG_KEY));

    functions.send = sendEnd;
    functions.flush = noFlush;
    CHECK(!drainChecked(&decoder));
    decoder.failed = false;
    functions.receive = endOfStream;
    CHECK(drainChecked(&decoder));

    std::puts("codec packet timestamps and random access: OK");
    return 0;
}
