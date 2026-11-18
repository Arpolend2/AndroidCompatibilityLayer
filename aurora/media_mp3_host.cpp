#include "media_mp3_host.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

class Mp3Worker : public QObject
{
    Q_OBJECT
public:
    QString path;
    megler_media_info info{};
    int16_t *output = nullptr;
    int maxFrames = 0;
    int64_t requestedTime = 0;
    int64_t outputTime = 0;
    int result = -1;

public slots:
    void open()
    {
        startDecoder();
        if (nextBuffer() != 1) {
            qWarning() << "Megler media: MP3 produced no initial audio";
            return;
        }
        const QAudioFormat fmt = buffer.format();
        if (fmt.codec() != QLatin1String("audio/pcm") || fmt.sampleType() != QAudioFormat::SignedInt ||
            fmt.sampleSize() != 16 || fmt.byteOrder() != QAudioFormat::LittleEndian ||
            fmt.sampleRate() <= 0 || fmt.sampleRate() > 192000 ||
            fmt.channelCount() < 1 || fmt.channelCount() > 2) {
            qWarning() << "Megler media: unsupported decoded MP3 format" << fmt;
            return;
        }
        info.sample_rate = fmt.sampleRate();
        info.channels = fmt.channelCount();
        const qint64 duration = decoder->duration();
        info.duration_us = duration > 0 && duration <= std::numeric_limits<int64_t>::max() / 1000
                               ? duration * 1000 : 0;
        result = 1;
    }

    void read()
    {
        result = -1;
        if (!output || maxFrames <= 0) {
            qWarning() << "Megler media: invalid audio read";
            return;
        }
        if (offset >= buffer.frameCount()) {
            const int status = nextBuffer();
            if (status != 1) {
                result = status;
                return;
            }
        }
        const int n = std::min(maxFrames, buffer.frameCount() - offset);
        std::memcpy(output, buffer.constData<char>() + static_cast<size_t>(offset) * info.channels * 2,
                    static_cast<size_t>(n) * info.channels * 2);
        outputTime = (framePosition / info.sample_rate) * 1000000 +
                     (framePosition % info.sample_rate) * 1000000 / info.sample_rate;
        offset += n;
        framePosition += n;
        result = n;
    }

    void seek()
    {
        result = -1;
        if (requestedTime < 0 || requestedTime > std::numeric_limits<int64_t>::max() / info.sample_rate) {
            qWarning() << "Megler media: invalid seek position";
            return;
        }
        const int64_t target = requestedTime * info.sample_rate / 1000000;
        buffer = QAudioBuffer();
        offset = 0;
        framePosition = 0;
        startDecoder();
        QElapsedTimer timer;
        timer.start();
        while (framePosition < target) {
            if (timer.elapsed() > 15000) {
                qWarning() << "Megler media: MP3 seek timed out";
                return;
            }
            if (offset >= buffer.frameCount()) {
                const int status = nextBuffer();
                if (status != 1) {
                    if (status == 0) result = 1;
                    return;
                }
            }
            const int64_t skip = std::min<int64_t>(target - framePosition, buffer.frameCount() - offset);
            offset += static_cast<int>(skip);
            framePosition += skip;
        }
        result = 1;
    }

    void close() { if (decoder) decoder->stop(); }

private:
    void startDecoder()
    {
        delete decoder;
        ended = false;
        decoder = new QAudioDecoder(this);
        connect(decoder, &QAudioDecoder::finished, this, [this] { ended = true; });
        decoder->setSourceFilename(path);
        decoder->start();
    }

    int nextBuffer()
    {
        if (!decoder->bufferAvailable() && !ended && decoder->error() == QAudioDecoder::NoError) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            connect(decoder, &QAudioDecoder::bufferReady, &loop, &QEventLoop::quit);
            connect(decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
            connect(decoder, static_cast<void (QAudioDecoder::*)(QAudioDecoder::Error)>(&QAudioDecoder::error),
                    &loop, &QEventLoop::quit);
            timer.start(10000);
            while (timer.isActive() && !decoder->bufferAvailable() && !ended &&
                   decoder->error() == QAudioDecoder::NoError)
                loop.exec();
        }
        if (!decoder->bufferAvailable()) {
            if (decoder->error() != QAudioDecoder::NoError) {
                qWarning() << "Megler media: MP3 decoding failed:" << decoder->errorString();
                return -1;
            }
            if (ended) return 0;
            qWarning() << "Megler media: MP3 decoder timed out";
            return -1;
        }
        buffer = decoder->read();
        offset = 0;
        const QAudioFormat fmt = buffer.format();
        if (!buffer.isValid() || buffer.frameCount() <= 0 || !buffer.constData() ||
            fmt.channelCount() < 1 || fmt.channelCount() > 2 ||
            static_cast<int64_t>(buffer.frameCount()) * fmt.channelCount() * 2 > buffer.byteCount() ||
            (info.sample_rate && (fmt.sampleRate() != info.sample_rate ||
             fmt.channelCount() != info.channels || fmt.sampleType() != QAudioFormat::SignedInt ||
             fmt.sampleSize() != 16 || fmt.byteOrder() != QAudioFormat::LittleEndian ||
             fmt.codec() != QLatin1String("audio/pcm")))) {
            qWarning() << "Megler media: invalid or changed MP3 output format";
            return -1;
        }
        return 1;
    }

    QAudioDecoder *decoder = nullptr;
    QAudioBuffer buffer;
    int offset = 0;
    int64_t framePosition = 0;
    bool ended = false;
};

struct Mp3Media {
    QThread thread;
    Mp3Worker *worker = new Mp3Worker;
    Mp3Media()
    {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }
    ~Mp3Media()
    {
        QMetaObject::invokeMethod(worker, "close", Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }
};

bool looksLikeMp3(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray h = file.read(3);
    if (h.size() == 3 && h == "ID3") return true;
    if (h.size() < 2) return false;
    const unsigned char a = static_cast<unsigned char>(h[0]);
    const unsigned char b = static_cast<unsigned char>(h[1]);
    return a == 0xff && (b & 0xe6) == 0xe2;
}

void *MEGLER_CB openMedia(void *, const char *path, megler_media_info *info)
{
    if (!path || !info || !looksLikeMp3(QString::fromUtf8(path))) {
        qWarning() << "Megler media: file is not a supported MP3";
        return nullptr;
    }
    Mp3Media *media = new Mp3Media;
    media->worker->path = QString::fromUtf8(path);
    if (!QMetaObject::invokeMethod(media->worker, "open", Qt::BlockingQueuedConnection) ||
        media->worker->result != 1) {
        qWarning() << "Megler media: could not open MP3";
        delete media;
        return nullptr;
    }
    *info = media->worker->info;
    return media;
}

int MEGLER_CB readAudio(void *, void *handle, int16_t *samples, int maxFrames, int64_t *time)
{
    if (!handle || !time) return -1;
    Mp3Worker *worker = static_cast<Mp3Media *>(handle)->worker;
    worker->output = samples;
    worker->maxFrames = maxFrames;
    if (!QMetaObject::invokeMethod(worker, "read", Qt::BlockingQueuedConnection)) {
        qWarning() << "Megler media: could not dispatch audio read";
        return -1;
    }
    if (worker->result > 0) *time = worker->outputTime;
    return worker->result;
}

bool MEGLER_CB seekMedia(void *, void *handle, int64_t time)
{
    if (!handle) return false;
    Mp3Worker *worker = static_cast<Mp3Media *>(handle)->worker;
    worker->requestedTime = time;
    if (!QMetaObject::invokeMethod(worker, "seek", Qt::BlockingQueuedConnection)) {
        qWarning() << "Megler media: could not dispatch MP3 seek";
        return false;
    }
    return worker->result == 1;
}

void MEGLER_CB closeMedia(void *, void *handle) { delete static_cast<Mp3Media *>(handle); }

}

megler_media_host auroraMp3MediaCallbacks()
{
    megler_media_host host = {};
    host.open = openMedia;
    host.read_audio = readAudio;
    host.seek = seekMedia;
    host.close = closeMedia;
    return host;
}

#include "media_mp3_host.moc"
