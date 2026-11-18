#include "../audio_output.h"
#include "../audio_format.h"

#include <QCoreApplication>
#include <QDebug>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static std::atomic<int> opened{0}, wrote{0}, sound{0};

static void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(QLatin1String("Megler audio: mixer opened output"))) ++opened;
    if (message == QLatin1String("Megler audio: mixer wrote PCM")) ++wrote;
    if (message == QLatin1String("Megler audio: mixer wrote nonzero PCM")) ++sound;
}

static int testFormats()
{
    const QAudioFormat input = audioInputFormat(44100);
    QAudioFormat alternate = input;
    alternate.setSampleRate(48000);
    alternate.setChannelCount(1);
    alternate.setSampleSize(8);
    alternate.setSampleType(QAudioFormat::UnSignedInt);
    alternate.setByteOrder(QAudioFormat::BigEndian);
    const auto onlyAlternate = [&alternate](const QAudioFormat &f) { return f == alternate; };
    QAudioFormat invalid = input;
    invalid.setSampleType(QAudioFormat::Float);
    CHECK(!audioConvertible(invalid));
    CHECK(audioChooseFormat(input, invalid, invalid, QList<int>() << 48000, onlyAlternate) == alternate);
    CHECK(audioChooseFormat(input, invalid, invalid, {}, onlyAlternate) == alternate);
    CHECK(audioChooseFormat(input, alternate, invalid, {}, onlyAlternate) == alternate);
    CHECK(audioChooseFormat(input, alternate, invalid, {}, [](const QAudioFormat &) { return true; }) == input);
    CHECK(!audioChooseFormat(input, invalid, invalid, {}, [](const QAudioFormat &) { return false; }).isValid());
    return 0;
}

static int testConversion()
{
    AudioQueue first(44100), split(44100);
    first.setAvailable(true);
    split.setAvailable(true);
    std::vector<int16_t> pcm(441 * 2);
    for (int i = 0; i < 441; ++i) {
        pcm[i * 2] = static_cast<int16_t>(i * 50 - 10000);
        pcm[i * 2 + 1] = static_cast<int16_t>(10000 - i * 40);
    }
    CHECK(first.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(int16_t)));
    CHECK(split.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(int16_t)));
    QAudioFormat format = audioInputFormat(44100);
    format.setSampleRate(48000);
    AudioConverter one(first, 44100, format), tiny(split, 44100, format);
    std::vector<char> whole(480 * 4), fragments(whole.size());
    bool queued, nonzero;
    one.read(whole.data(), whole.size(), queued, nonzero);
    CHECK(queued && nonzero);
    for (std::size_t i = 0; i < fragments.size(); ++i)
        tiny.read(fragments.data() + i, 1, queued, nonzero);
    CHECK(whole == fragments);
    for (int i = 0; i < 480; ++i) {
        int16_t l, r;
        std::memcpy(&l, whole.data() + i * 4, 2);
        std::memcpy(&r, whole.data() + i * 4 + 2, 2);
        const int source = i * 44100 / 48000;
        CHECK(l == pcm[source * 2] && r == pcm[source * 2 + 1]);
    }

    QAudioFormat mono = audioInputFormat(44100);
    mono.setChannelCount(1);
    mono.setSampleSize(8);
    mono.setSampleType(QAudioFormat::UnSignedInt);
    AudioQueue unsignedQueue(44100);
    unsignedQueue.setAvailable(true);
    const int16_t source[] = {32767, 32767, -32768, -32768, 32767, -32768};
    CHECK(unsignedQueue.write(reinterpret_cast<const char *>(source), sizeof(source)));
    AudioConverter unsignedOutput(unsignedQueue, 44100, mono);
    unsigned char result[4] = {};
    unsignedOutput.read(reinterpret_cast<char *>(result), sizeof(result), queued, nonzero);
    CHECK(queued && nonzero && result[0] == 255 && result[1] == 0 && result[2] == 128 && result[3] == 128);

    QAudioFormat big = audioInputFormat(44100);
    big.setSampleSize(24);
    big.setByteOrder(QAudioFormat::BigEndian);
    AudioQueue bigQueue(44100);
    bigQueue.setAvailable(true);
    CHECK(bigQueue.write(reinterpret_cast<const char *>(source), 4));
    AudioConverter bigOutput(bigQueue, 44100, big);
    unsigned char bigBytes[6] = {};
    bigOutput.read(reinterpret_cast<char *>(bigBytes), sizeof(bigBytes), queued, nonzero);
    const unsigned char expected[] = {0x7f, 0xff, 0x00, 0x7f, 0xff, 0x00};
    CHECK(std::memcmp(bigBytes, expected, sizeof(expected)) == 0);

    QAudioFormat unsigned32 = audioInputFormat(44100);
    unsigned32.setSampleSize(32);
    unsigned32.setSampleType(QAudioFormat::UnSignedInt);
    unsigned32.setByteOrder(QAudioFormat::BigEndian);
    AudioQueue unsigned32Queue(44100);
    unsigned32Queue.setAvailable(true);
    const int16_t extremes[] = {-32768, 32767};
    CHECK(unsigned32Queue.write(reinterpret_cast<const char *>(extremes), sizeof(extremes)));
    AudioConverter unsigned32Output(unsigned32Queue, 44100, unsigned32);
    unsigned char unsigned32Bytes[8] = {};
    unsigned32Output.read(reinterpret_cast<char *>(unsigned32Bytes), sizeof(unsigned32Bytes), queued, nonzero);
    const unsigned char expectedUnsigned32[] = {0, 0, 0, 0, 0xff, 0xff, 0, 0};
    CHECK(std::memcmp(unsigned32Bytes, expectedUnsigned32, sizeof(expectedUnsigned32)) == 0);

    QAudioFormat floating = audioInputFormat(44100);
    floating.setSampleType(QAudioFormat::Float);
    floating.setSampleSize(32);
    AudioQueue floatQueue(44100);
    floatQueue.setAvailable(true);
    CHECK(floatQueue.write(reinterpret_cast<const char *>(source + 2), 4));
    AudioConverter floatOutput(floatQueue, 44100, floating);
    float floats[2] = {};
    floatOutput.read(reinterpret_cast<char *>(floats), sizeof(floats), queued, nonzero);
    CHECK(floats[0] == -1.0f && floats[1] == -1.0f);

    AudioQueue downQueue(44100);
    downQueue.setAvailable(true);
    CHECK(downQueue.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(int16_t)));
    format.setSampleRate(22050);
    AudioConverter down(downQueue, 44100, format);
    std::vector<char> downBytes(220 * 4);
    down.read(downBytes.data(), downBytes.size(), queued, nonzero);
    for (int i = 0; i < 220; ++i) {
        int16_t l;
        std::memcpy(&l, downBytes.data() + i * 4, 2);
        CHECK(l == pcm[i * 4]);
    }

    AudioQueue fractionalQueue(44100);
    fractionalQueue.setAvailable(true);
    CHECK(fractionalQueue.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(int16_t)));
    format.setSampleRate(32000);
    AudioConverter fractional(fractionalQueue, 44100, format);
    std::vector<char> fractionalBytes(320 * 4);
    fractional.read(fractionalBytes.data(), fractionalBytes.size(), queued, nonzero);
    for (int i = 0; i < 320; ++i) {
        int16_t l;
        std::memcpy(&l, fractionalBytes.data() + i * 4, 2);
        CHECK(l == pcm[(i * 44100 / 32000) * 2]);
    }

    AudioQueue clockQueue(44100);
    clockQueue.setAvailable(true);
    format.setSampleRate(48000);
    AudioConverter clock(clockQueue, 44100, format);
    std::vector<int16_t> step(441 * 2);
    for (int block = 0; block < 100; ++block) {
        for (int i = 0; i < 441; ++i)
            step[i * 2] = step[i * 2 + 1] = static_cast<int16_t>((block * 441 + i) % 30000);
        CHECK(clockQueue.write(reinterpret_cast<const char *>(step.data()), step.size() * sizeof(int16_t)));
        clock.read(whole.data(), whole.size(), queued, nonzero);
        for (int i = 0; i < 480; ++i) {
            int16_t l;
            std::memcpy(&l, whole.data() + i * 4, 2);
            CHECK(l == static_cast<int16_t>(((block * 480 + i) * 44100 / 48000) % 30000));
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CHECK(testFormats() == 0);
    CHECK(testConversion() == 0);
    qInstallMessageHandler(capture);
    for (int i = 0; i < 10; ++i) {
        AuroraAudioOutput output;
        const megler_audio_host host = output.callbacks();
        CHECK(host.user && host.open && host.write && !host.route);
        CHECK(!host.open(host.user, 0));
        CHECK(host.open(host.user, 44100));
        CHECK(!host.open(host.user, 44100));
        int16_t samples[882] = {};
        std::thread writer([&] { host.write(host.user, samples, 441); });
        output.shutdown();
        writer.join();
        output.shutdown();
        CHECK(!host.open(host.user, 44100));
        const auto start = std::chrono::steady_clock::now();
        for (int j = 0; j < 5; ++j) host.write(host.user, samples, 441);
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(40));
    }
    AuroraAudioOutput unopened;
    const megler_audio_host unopenedHost = unopened.callbacks();
    unopened.shutdown();
    CHECK(!unopenedHost.open(unopenedHost.user, 44100));
    opened = wrote = sound = 0;
    AuroraAudioOutput diagnostic;
    const megler_audio_host diagnosticHost = diagnostic.callbacks();
    CHECK(diagnosticHost.open(diagnosticHost.user, 44100));
    int16_t silent[882] = {};
    diagnosticHost.write(diagnosticHost.user, silent, 441);
    CHECK(opened == 1 && wrote == 1 && sound == 0);
    silent[1] = 1;
    diagnosticHost.write(diagnosticHost.user, silent, 441);
    diagnosticHost.write(diagnosticHost.user, silent, 441);
    CHECK(opened == 1 && wrote == 1 && sound == 1);
    diagnostic.shutdown();
    qInstallMessageHandler(nullptr);
    std::puts("audio output callback tests passed");
    return 0;
}
