#include "../codec_audio.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return false; \
} } while (0)

struct Packet {
    QByteArray data;
    qint64 time;
};

static bool testStream(const megler_codec_host &host, const QString &path, int kind)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    CHECK(bytes.size() >= 60 && bytes.left(8) == "MGMEDIA1");
    const uchar *base = reinterpret_cast<const uchar *>(bytes.constData());
    const quint32 count = qFromLittleEndian<quint32>(base + 56);
    int at = 60;
    QByteArray config;
    std::vector<Packet> packets;
    for (quint32 i = 0; i < count; ++i) {
        CHECK(at <= bytes.size() - 16);
        const quint32 flags = qFromLittleEndian<quint32>(base + at);
        const quint32 size = qFromLittleEndian<quint32>(base + at + 4);
        const qint64 time = qFromLittleEndian<qint64>(base + at + 8);
        at += 16;
        CHECK(size <= static_cast<quint32>(bytes.size() - at));
        if (flags & 2) config += bytes.mid(at, size);
        else packets.push_back(Packet{bytes.mid(at, size), time});
        at += static_cast<int>(size);
    }
    CHECK(!packets.empty());
    megler_codec_format format = {};
    format.sample_rate = 44100;
    format.channels = 2;
    format.config = reinterpret_cast<const uint8_t *>(config.constData());
    format.config_size = static_cast<size_t>(config.size());
    void *decoder = host.open(host.user, kind, &format);
    CHECK(decoder);
    std::vector<int16_t> pcm;
    int64_t last = -1;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Packet &p = packets[i];
        CHECK(host.push(host.user, decoder, reinterpret_cast<const uint8_t *>(p.data.constData()),
                        static_cast<size_t>(p.data.size()), p.time + 1000000));
        if (i + 1 == packets.size()) CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
        megler_codec_sound sound = {};
        while (host.pull_sound(host.user, decoder, &sound)) {
            CHECK(sound.samples && sound.frames > 0 && sound.sample_rate == 44100 && sound.channels == 2);
            CHECK(sound.time_us >= 1000000 && sound.time_us >= last);
            last = sound.time_us;
            pcm.insert(pcm.end(), sound.samples, sound.samples + sound.frames * sound.channels);
        }
    }
    const std::size_t frames = pcm.size() / 2;
    CHECK(frames > 18000 && frames < 35000);
    double sum = 0;
    int crossings = 0;
    for (std::size_t i = frames / 4; i < frames * 3 / 4; ++i) {
        const double s = pcm[i * 2];
        sum += s * s;
        if (pcm[(i - 1) * 2] < 0 && s >= 0) ++crossings;
    }
    const double rms = std::sqrt(sum / (frames / 2));
    const double hz = crossings * 44100.0 / (frames / 2);
    CHECK(rms > 4000 && rms < 12000 && hz > 430 && hz < 450);

    host.flush(host.user, decoder);
    megler_codec_sound sound = {};
    CHECK(!host.pull_sound(host.user, decoder, &sound));
    if (kind == MEGLER_CODEC_MP3) {
        CHECK(packets.size() >= 2);
        const QByteArray combined = packets[0].data + packets[1].data;
        CHECK(host.push(host.user, decoder, reinterpret_cast<const uint8_t *>(combined.constData()),
                        static_cast<size_t>(combined.size()), 3000000));
        CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
        int blocks = 0;
        int64_t previous = -1;
        while (host.pull_sound(host.user, decoder, &sound)) {
            CHECK(sound.time_us >= 3000000 && sound.time_us >= previous);
            previous = sound.time_us;
            ++blocks;
        }
        CHECK(blocks == 2 && previous > 3000000);
        host.flush(host.user, decoder);
        const uint8_t bad[] = {0, 1, 2, 3};
        CHECK(!host.push(host.user, decoder, bad, sizeof(bad), 0));
        host.flush(host.user, decoder);
    }
    const Packet &p = packets.front();
    CHECK(host.push(host.user, decoder, reinterpret_cast<const uint8_t *>(p.data.constData()),
                    static_cast<size_t>(p.data.size()), 2000000));
    CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
    while (host.pull_sound(host.user, decoder, &sound)) CHECK(sound.time_us >= 2000000);
    CHECK(host.push(host.user, decoder, reinterpret_cast<const uint8_t *>(p.data.constData()),
                    static_cast<size_t>(p.data.size()), 4000000));
    CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
    while (host.pull_sound(host.user, decoder, &sound)) CHECK(sound.time_us >= 4000000);
    host.close(host.user, decoder);
    std::printf("codec %d: %zu frames, %.1f Hz, RMS %.0f\n", kind, frames, hz, rms);
    return true;
}

static bool testVideoStream(const megler_codec_host &host, const QString &path, const QString &audioPath, int kind,
                            bool duplicateTimes = false)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    CHECK(bytes.size() >= 60 && bytes.left(8) == "MGMEDIA1");
    const uchar *base = reinterpret_cast<const uchar *>(bytes.constData());
    const quint32 count = qFromLittleEndian<quint32>(base + 56);
    megler_codec_format format = {};
    format.width = static_cast<int>(qFromLittleEndian<quint32>(base + 40));
    format.height = static_cast<int>(qFromLittleEndian<quint32>(base + 44));
    int at = 60;
    QByteArray config;
    std::vector<Packet> packets;
    for (quint32 i = 0; i < count; ++i) {
        CHECK(at <= bytes.size() - 16);
        const quint32 flags = qFromLittleEndian<quint32>(base + at);
        const quint32 size = qFromLittleEndian<quint32>(base + at + 4);
        const qint64 time = qFromLittleEndian<qint64>(base + at + 8);
        at += 16;
        CHECK(size <= static_cast<quint32>(bytes.size() - at));
        if (flags & 2) config += bytes.mid(at, size);
        else packets.push_back(Packet{bytes.mid(at, size), time});
        at += static_cast<int>(size);
    }
    CHECK(format.width > 0 && format.height > 0 && !config.isEmpty() && !packets.empty());
    if (duplicateTimes) {
        for (Packet &packet : packets) {
            const qint64 number = (packet.time * 30 + 500000) / 1000000;
            packet.time = 1000000 + (number / 2) * 33333;
        }
    }
    format.config = reinterpret_cast<const uint8_t *>(config.constData());
    format.config_size = static_cast<size_t>(config.size());
    megler_codec_format invalidVideo = format;
    invalidVideo.width = 8193;
    CHECK(!host.open(host.user, kind, &invalidVideo));
    void *decoder = host.open(host.user, kind, &format);
    CHECK(decoder);
    QFile audioFile(audioPath);
    CHECK(audioFile.open(QIODevice::ReadOnly));
    const QByteArray audioBytes = audioFile.readAll();
    CHECK(audioBytes.size() >= 60 && audioBytes.left(8) == "MGMEDIA1");
    const uchar *audioBase = reinterpret_cast<const uchar *>(audioBytes.constData());
    const quint32 audioCount = qFromLittleEndian<quint32>(audioBase + 56);
    int audioAt = 60;
    QByteArray audioConfig;
    std::vector<Packet> audioPackets;
    for (quint32 i = 0; i < audioCount; ++i) {
        CHECK(audioAt <= audioBytes.size() - 16);
        const quint32 flags = qFromLittleEndian<quint32>(audioBase + audioAt);
        const quint32 size = qFromLittleEndian<quint32>(audioBase + audioAt + 4);
        const qint64 time = qFromLittleEndian<qint64>(audioBase + audioAt + 8);
        audioAt += 16;
        CHECK(size <= static_cast<quint32>(audioBytes.size() - audioAt));
        if (flags & 2) audioConfig += audioBytes.mid(audioAt, size);
        else audioPackets.push_back(Packet{audioBytes.mid(audioAt, size), time});
        audioAt += static_cast<int>(size);
    }
    megler_codec_format audioFormat = {};
    audioFormat.sample_rate = static_cast<int>(qFromLittleEndian<quint32>(audioBase + 48));
    audioFormat.channels = static_cast<int>(qFromLittleEndian<quint32>(audioBase + 52));
    audioFormat.config = reinterpret_cast<const uint8_t *>(audioConfig.constData());
    audioFormat.config_size = static_cast<size_t>(audioConfig.size());
    void *audioDecoder = host.open(host.user, MEGLER_CODEC_AAC, &audioFormat);
    CHECK(audioDecoder && !audioPackets.empty());
    int pictures = 0;
    std::size_t audioFrames = 0;
    int64_t previousTime = -1;
    auto pullPictures = [&]() {
        megler_codec_picture picture = {};
        void *handle;
        while ((handle = host.pull_picture(host.user, decoder, &picture)) != nullptr) {
            bool valid = picture.format == MEGLER_CODEC_RGBA && picture.width == format.width &&
                         picture.height == format.height && picture.data &&
                         picture.stride >= picture.width * 4 && picture.time_us >= previousTime;
            if (valid) {
                const int number = duplicateTimes ? pictures :
                    static_cast<int>((picture.time_us * 30 + 500000) / 1000000);
                const int expected = (298 * (16 + 8 * number - 16) + 128) >> 8;
                const uint8_t *center = picture.data +
                    static_cast<size_t>(picture.height / 2) * picture.stride +
                    static_cast<size_t>(picture.width / 2) * 4;
                valid = number == pictures &&
                        (!duplicateTimes || picture.time_us == 1000000 + (number / 2) * 33333) &&
                        std::abs(center[0] - expected) < 12 &&
                        std::abs(center[1] - expected) < 12 && std::abs(center[2] - expected) < 12 &&
                        center[3] == 255;
            }
            if (valid) {
                previousTime = picture.time_us;
                ++pictures;
            }
            host.release_picture(host.user, handle);
            if (!valid) return false;
        }
        return true;
    };
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const Packet &packet = packets[i];
        CHECK(host.push(host.user, decoder,
                        reinterpret_cast<const uint8_t *>(packet.data.constData()),
                        static_cast<size_t>(packet.data.size()), packet.time));
        CHECK(pullPictures());
        if (i < audioPackets.size()) {
            const Packet &audioPacket = audioPackets[i];
            CHECK(host.push(host.user, audioDecoder,
                            reinterpret_cast<const uint8_t *>(audioPacket.data.constData()),
                            static_cast<size_t>(audioPacket.data.size()), audioPacket.time));
            megler_codec_sound sound = {};
            while (host.pull_sound(host.user, audioDecoder, &sound)) {
                CHECK(sound.frames > 0 && sound.sample_rate == audioFormat.sample_rate &&
                      sound.channels == audioFormat.channels);
                audioFrames += static_cast<size_t>(sound.frames);
            }
        }
    }
    CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
    CHECK(pullPictures());
    CHECK(pictures == static_cast<int>(packets.size()));
    const int decodedPictures = pictures;
    CHECK(auroraMediaCodecDrainChecked(host.user, audioDecoder));
    megler_codec_sound audioSound = {};
    while (host.pull_sound(host.user, audioDecoder, &audioSound)) {
        CHECK(audioSound.frames > 0 && audioSound.sample_rate == audioFormat.sample_rate &&
              audioSound.channels == audioFormat.channels);
        audioFrames += static_cast<size_t>(audioSound.frames);
    }
    CHECK(audioFrames > 18000);
    host.flush(host.user, decoder);
    host.flush(host.user, audioDecoder);
    megler_codec_picture empty = {};
    CHECK(!host.pull_picture(host.user, decoder, &empty));
    CHECK(!host.pull_sound(host.user, audioDecoder, &audioSound));
    pictures = 0;
    previousTime = -1;
    CHECK(host.push(host.user, decoder,
                    reinterpret_cast<const uint8_t *>(packets.front().data.constData()),
                    static_cast<size_t>(packets.front().data.size()), packets.front().time));
    CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
    CHECK(pullPictures() && pictures == 1);
    host.flush(host.user, decoder);
    CHECK(host.push(host.user, decoder,
                    reinterpret_cast<const uint8_t *>(packets.front().data.constData()),
                    static_cast<size_t>(packets.front().data.size()), packets.front().time));
    CHECK(auroraMediaCodecDrainChecked(host.user, decoder));
    megler_codec_picture retained = {};
    void *retainedHandle = host.pull_picture(host.user, decoder, &retained);
    CHECK(retainedHandle && retained.data && retained.stride >= retained.width * 4);
    const QByteArray firstRow(reinterpret_cast<const char *>(retained.data), retained.width * 4);
    host.flush(host.user, decoder);
    host.close(host.user, audioDecoder);
    host.close(host.user, decoder);
    CHECK(firstRow == QByteArray(reinterpret_cast<const char *>(retained.data), retained.width * 4));
    host.release_picture(host.user, retainedHandle);
    std::printf("video codec %d/AAC: %d RGBA pictures at %dx%d and %zu audio frames\n",
                kind, decodedPictures, format.width, format.height, audioFrames);
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 1;
    const megler_codec_host host = auroraMediaCodecCallbacks();
    if (!host.supports || !host.open || !host.push || !host.pull_picture || !host.release_picture ||
        !host.pull_sound || !host.drain || !host.flush || !host.close) return 1;
    if (!host.supports(host.user, MEGLER_CODEC_H264) || !host.supports(host.user, MEGLER_CODEC_H265)) return 1;
    if (!host.supports(host.user, MEGLER_CODEC_AAC) || !host.supports(host.user, MEGLER_CODEC_MP3)) {
        std::fprintf(stderr, "FFmpeg 8 AAC/H.264 or minimp3 decoder unavailable\n");
        return 1;
    }
    megler_codec_format invalid = {};
    invalid.sample_rate = 44100;
    invalid.channels = 2;
    const uint8_t badAac[] = {0};
    invalid.config = badAac;
    invalid.config_size = sizeof(badAac);
    if (host.open(host.user, MEGLER_CODEC_AAC, &invalid)) return 1;
    const QString dir = QString::fromLocal8Bit(argv[1]);
    return testVideoStream(host, dir + QStringLiteral("/avc.mgau"), dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_H264) &&
           testVideoStream(host, dir + QStringLiteral("/hevc.mgau"), dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_H265) &&
           testVideoStream(host, dir + QStringLiteral("/avc.mgau"), dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_H264, true) &&
           testVideoStream(host, dir + QStringLiteral("/hevc.mgau"), dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_H265, true) &&
           testVideoStream(host, dir + QStringLiteral("/hevc10.mgau"), dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_H265) &&
           testStream(host, dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_AAC) &&
           testStream(host, dir + QStringLiteral("/mp3.mgau"), MEGLER_CODEC_MP3) ? 0 : 1;
}
