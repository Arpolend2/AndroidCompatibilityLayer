#include "../media_mp3_host.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryFile>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", qPrintable(message));
    });
    if (argc != 2) return 2;
    const megler_media_host host = auroraMp3MediaCallbacks();
    const QByteArray path = QDir(QString::fromLocal8Bit(argv[1])).filePath("tone.mp3").toUtf8();
    const QByteArray nonMp3 = QDir(QString::fromLocal8Bit(argv[1])).filePath("tone.m4a").toUtf8();
    megler_media_info info{};
    assert(!host.open(host.user, nonMp3.constData(), &info));
    QTemporaryFile damaged;
    assert(damaged.open());
    assert(damaged.write("ID3broken", 9) == 9 && damaged.flush());
    const QByteArray damagedPath = damaged.fileName().toUtf8();
    assert(!host.open(host.user, damagedPath.constData(), &info));
    void *media = host.open(host.user, path.constData(), &info);
    assert(media && info.width == 0 && info.height == 0 && info.sample_rate == 44100 && info.channels == 1);
    assert(info.duration_us > 0);
    std::vector<int16_t> pcm;
    int16_t block[777];
    int64_t time = -1;
    for (;;) {
        int n = host.read_audio(host.user, media, block, 777, &time);
        assert(n >= 0);
        if (!n) break;
        assert(time == static_cast<int64_t>(pcm.size()) * 1000000 / info.sample_rate);
        pcm.insert(pcm.end(), block, block + n);
    }
    assert(pcm.size() > 10000);
    assert(std::any_of(pcm.begin(), pcm.end(), [](int16_t sample) { return sample != 0; }));
    const int64_t seekUs = 100000;
    const size_t index = static_cast<size_t>(seekUs * info.sample_rate / 1000000);
    assert(host.seek(host.user, media, seekUs));
    const int n = host.read_audio(host.user, media, block, 777, &time);
    assert(n > 0 && time == seekUs);
    assert(std::equal(block, block + n, pcm.begin() + index));
    assert(host.seek(host.user, media, 0));
    assert(host.read_audio(host.user, media, block, 777, &time) > 0 && time == 0);
    assert(!host.seek(host.user, media, -1));
    host.close(host.user, media);
    std::puts("Qt MP3 media host passed");
    return 0;
}
