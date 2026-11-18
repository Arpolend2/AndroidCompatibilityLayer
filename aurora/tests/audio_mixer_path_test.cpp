#include "../audio_output.h"
#include "../audio_mp3_codec.h"
#include "../../core/src/drivers/mg_java.h"
#include "../../core/src/ndk/mg_audio.h"
#include "../../core/src/ld/mg_ld.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static megler_run_config config{};
static megler_audio_host output{};
static std::atomic<bool> heard{false};

extern "C" const megler_run_config *mg_run_config() { return &config; }
extern "C" bool mg_flag(const char *) { return false; }
extern "C" const char *megler_data_dir() { return ""; }
extern "C" mg_addr mg_code_entry(mg_addr fn) { return fn; }

static bool MEGLER_CB openOutput(void *, int rate) { return output.open(output.user, rate); }
static void MEGLER_CB writeOutput(void *, const int16_t *samples, int frames)
{
    for (int i = 0; i < frames * 2; ++i) if (samples[i]) { heard = true; break; }
    output.write(output.user, samples, frames);
}

static uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(file.size() >= 60 && std::memcmp(file.data(), "MGMEDIA1", 8) == 0);
    megler_codec_host codecHost = auroraMp3CodecCallbacks();
    mg_java_media_set_host(&codecHost);
    CHECK(mg_mcodec_supported("audio/mpeg") && !mg_mcodec_supported("audio/mp4a-latm"));
    mg_mcodec *codec = mg_mcodec_create("audio/mpeg");
    const u8 *csd[3] = {nullptr, nullptr, nullptr};
    const size_t csdSizes[3] = {0, 0, 0};
    CHECK(codec && mg_mcodec_configure(codec, 0, 0, 0, 0, csd, csdSizes));
    AuroraAudioOutput *bridge = new AuroraAudioOutput;
    output = bridge->callbacks();
    config.audio.open = openOutput;
    config.audio.write = writeOutput;
    mg_pcm *stream = mg_pcm_open(44100, 2, 1, 100);
    CHECK(stream);
    mg_pcm_play(stream, true);
    const uint32_t count = le32(file.data() + 56);
    std::size_t offset = 60;
    int totalFrames = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(offset <= file.size() && file.size() - offset >= 16);
        const uint32_t flags = le32(file.data() + offset);
        const uint32_t size = le32(file.data() + offset + 4);
        offset += 16;
        CHECK(size <= file.size() - offset);
        mg_mcodec_decode(codec, file.data() + offset, size, static_cast<int64_t>(i) * 26122, flags);
        offset += size;
        i64 info[3]{};
        size_t bytes = 0;
        u8 *pcm;
        while ((pcm = mg_mcodec_next_pcm(codec, info, &bytes))) {
            CHECK(info[1] == 44100 && info[2] == 2 && bytes % 4 == 0);
            const int frames = static_cast<int>(bytes / 4);
            CHECK(mg_pcm_write(stream, pcm, frames, true) == frames);
            totalFrames += frames;
            std::free(pcm);
        }
    }
    CHECK(totalFrames > 18000);
    for (int i = 0; i < 200 && !heard; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(heard);
    CHECK(mg_pcm_position(stream) > 0);
    mg_pcm_close(stream);
    mg_mcodec_destroy(codec);
    mg_java_media_set_host(nullptr);
    bridge->shutdown();
    std::puts("MP3 MediaCodec PCM reached the mixer and Aurora output callback");
    return 0;
}
