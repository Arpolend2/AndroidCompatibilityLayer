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
        if (flags & 2) config = bytes.mid(at, size);
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
        if (i + 1 == packets.size()) host.drain(host.user, decoder);
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
        host.drain(host.user, decoder);
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
    host.drain(host.user, decoder);
    while (host.pull_sound(host.user, decoder, &sound)) CHECK(sound.time_us >= 2000000);
    CHECK(host.push(host.user, decoder, reinterpret_cast<const uint8_t *>(p.data.constData()),
                    static_cast<size_t>(p.data.size()), 4000000));
    host.drain(host.user, decoder);
    while (host.pull_sound(host.user, decoder, &sound)) CHECK(sound.time_us >= 4000000);
    host.close(host.user, decoder);
    std::printf("codec %d: %zu frames, %.1f Hz, RMS %.0f\n", kind, frames, hz, rms);
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 1;
    const megler_codec_host host = auroraAudioCodecCallbacks();
    if (!host.supports || !host.open || !host.push || !host.pull_sound || !host.drain || !host.flush || !host.close) return 1;
    if (host.supports(host.user, MEGLER_CODEC_H264) || host.supports(host.user, MEGLER_CODEC_H265)) return 1;
    if (!host.supports(host.user, MEGLER_CODEC_AAC) || !host.supports(host.user, MEGLER_CODEC_MP3)) {
        std::fprintf(stderr, "FFmpeg 7 AAC/MP3 decoders unavailable\n");
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
    return testStream(host, dir + QStringLiteral("/aac.mgau"), MEGLER_CODEC_AAC) &&
           testStream(host, dir + QStringLiteral("/mp3.mgau"), MEGLER_CODEC_MP3) ? 0 : 1;
}
