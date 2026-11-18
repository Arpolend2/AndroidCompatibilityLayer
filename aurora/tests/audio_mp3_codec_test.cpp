#include "../audio_mp3_codec.h"
#include "../audio_queue.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(file.size() >= 60 && std::memcmp(file.data(), "MGMEDIA1", 8) == 0);
    const uint32_t count = le32(file.data() + 56);
    std::vector<std::vector<uint8_t>> packets;
    std::size_t offset = 60;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(offset <= file.size() && file.size() - offset >= 16);
        const uint32_t flags = le32(file.data() + offset);
        const uint32_t size = le32(file.data() + offset + 4);
        offset += 16;
        CHECK(size <= file.size() - offset);
        if (!(flags & 2) && size) packets.emplace_back(file.begin() + offset, file.begin() + offset + size);
        offset += size;
    }
    CHECK(packets.size() >= 2);
    const megler_codec_host host = auroraMp3CodecCallbacks();
    CHECK(host.supports(nullptr, MEGLER_CODEC_MP3));
    CHECK(!host.supports(nullptr, MEGLER_CODEC_AAC) && !host.supports(nullptr, MEGLER_CODEC_H264));
    megler_codec_format format{};
    format.sample_rate = 44100;
    format.channels = 2;
    CHECK(!host.open(nullptr, MEGLER_CODEC_AAC, &format));
    void *decoder = host.open(nullptr, MEGLER_CODEC_MP3, &format);
    CHECK(decoder);
    AudioQueue queue(44100);
    queue.setAvailable(true);
    std::vector<int16_t> pcm;
    int64_t time = 1000000, last = -1;
    int blocks = 0;
    for (const auto &packet : packets) {
        CHECK(host.push(nullptr, decoder, packet.data(), packet.size(), time));
        time += 26122;
        megler_codec_sound sound{};
        while (host.pull_sound(nullptr, decoder, &sound)) {
            CHECK(sound.frames > 0 && sound.sample_rate == 44100 && sound.channels == 2);
            CHECK(sound.time_us >= 1000000 && sound.time_us >= last);
            last = sound.time_us;
            const std::size_t bytes = static_cast<std::size_t>(sound.frames) * sound.channels * sizeof(int16_t);
            CHECK(queue.write(reinterpret_cast<const char *>(sound.samples), bytes));
            std::vector<char> read(bytes);
            CHECK(queue.read(read.data(), bytes) == bytes);
            CHECK(std::memcmp(read.data(), sound.samples, bytes) == 0);
            pcm.insert(pcm.end(), sound.samples, sound.samples + sound.frames * sound.channels);
            ++blocks;
        }
    }
    host.drain(nullptr, decoder);
    const std::size_t frames = pcm.size() / 2;
    CHECK(frames > 18000 && frames < 35000 && blocks > 2);
    double sum = 0;
    int crossings = 0;
    for (std::size_t i = frames / 4; i < frames * 3 / 4; ++i) {
        double s = pcm[i * 2];
        sum += s * s;
        if (pcm[(i - 1) * 2] < 0 && s >= 0) ++crossings;
    }
    const double rms = std::sqrt(sum / (frames / 2));
    const double hz = crossings * 44100.0 / (frames / 2);
    CHECK(rms > 4000 && rms < 12000 && hz > 430 && hz < 450);
    host.flush(nullptr, decoder);
    std::vector<uint8_t> combined = packets[0];
    combined.insert(combined.end(), packets[1].begin(), packets[1].end());
    CHECK(host.push(nullptr, decoder, combined.data(), combined.size(), 3000000));
    megler_codec_sound sound{};
    int combinedBlocks = 0;
    while (host.pull_sound(nullptr, decoder, &sound)) {
        CHECK(sound.time_us >= 3000000 && (combinedBlocks == 0 || sound.time_us > 3000000));
        ++combinedBlocks;
    }
    CHECK(combinedBlocks == 2);
    host.flush(nullptr, decoder);
    const uint8_t bad[] = {0, 1, 2, 3};
    CHECK(!host.push(nullptr, decoder, bad, sizeof(bad), 0));
    CHECK(!host.push(nullptr, decoder, packets[0].data(), packets[0].size(), 0));
    host.flush(nullptr, decoder);
    CHECK(!host.push(nullptr, decoder, packets[0].data(), packets[0].size(), std::numeric_limits<int64_t>::max()));
    host.flush(nullptr, decoder);
    CHECK(host.push(nullptr, decoder, packets[0].data(), packets[0].size(), 4000000));
    host.drain(nullptr, decoder);
    CHECK(host.pull_sound(nullptr, decoder, &sound) && sound.time_us == 4000000);
    host.flush(nullptr, decoder);
    CHECK(!host.pull_sound(nullptr, decoder, &sound));
    uint32_t random = 0x12345678;
    for (int i = 0; i < 500; ++i) {
        std::vector<uint8_t> damaged = packets[static_cast<std::size_t>(i) % packets.size()];
        CHECK(damaged.size() > 4);
        for (int j = 0; j < 8; ++j) {
            random = random * 1664525u + 1013904223u;
            const std::size_t at = 4 + random % (damaged.size() - 4);
            random = random * 1664525u + 1013904223u;
            damaged[at] ^= static_cast<uint8_t>(random >> 24);
        }
        host.push(nullptr, decoder, damaged.data(), damaged.size(), 0);
        while (host.pull_sound(nullptr, decoder, &sound))
            CHECK(sound.frames > 0 && sound.frames <= 1152 && sound.channels > 0 && sound.channels <= 2);
        host.flush(nullptr, decoder);
    }
    host.close(nullptr, decoder);
    std::printf("MP3: %zu frames, %.1f Hz, RMS %.0f; PCM queue matches\n", frames, hz, rms);
    return 0;
}
