#include "audio_output.h"
#include "audio_queue.h"
#include "audio_format.h"

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QDebug>
#include <QIODevice>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>

namespace {

class AudioSource : public QIODevice
{
public:
    AudioSource(AudioQueue &queue, int rate, const QAudioFormat &format)
        : m_converter(queue, rate, format) { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 4096 + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 size) override
    {
        if (size <= 0) return 0;
        bool queued, nonzero;
        m_converter.read(data, static_cast<std::size_t>(size), queued, nonzero);
        if (queued && !m_reportedData.exchange(true)) {
            qInfo() << "AndroidCompatibilityLayer audio: Qt backend read queued PCM";
        }
        if (nonzero && !m_reportedSound.exchange(true))
            qInfo() << "AndroidCompatibilityLayer audio: Qt backend consumed nonzero source PCM";
        return size;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    AudioConverter m_converter;
    std::atomic<bool> m_reportedData{false};
    std::atomic<bool> m_reportedSound{false};
};

class AudioThread : public QThread
{
public:
    AudioThread(AudioQueue &queue, int rate) : m_queue(queue), m_rate(rate) {}
    void stop()
    {
        m_stopping.store(true);
        quit();
    }

protected:
    void run() override
    {
        const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
        if (device.isNull()) {
            qWarning() << "AndroidCompatibilityLayer audio: no default Qt output device";
            return;
        }
        const QAudioFormat input = audioInputFormat(m_rate);
        const QAudioFormat nearest = device.nearestFormat(input);
        const QAudioFormat preferred = device.preferredFormat();
        const QAudioFormat format = audioChooseFormat(input, nearest, preferred, device.supportedSampleRates(),
                                                       [&device](const QAudioFormat &candidate) {
            return device.isFormatSupported(candidate);
        });
        if (!format.isValid()) {
            qWarning() << "AndroidCompatibilityLayer audio: no supported convertible PCM format; nearest" << nearest
                       << "preferred" << preferred << "sample rates" << device.supportedSampleRates()
                       << "channels" << device.supportedChannelCounts() << "sizes" << device.supportedSampleSizes()
                       << "types" << device.supportedSampleTypes() << "byte orders" << device.supportedByteOrders()
                       << "codecs" << device.supportedCodecs();
            return;
        }
        qInfo() << "AndroidCompatibilityLayer audio: selected Qt output format" << format;
        AudioSource source(m_queue, m_rate, format);
        QAudioOutput output(device, format);
        output.setBufferSize(format.sampleRate() * format.bytesPerFrame() / 10);
        QObject::connect(&output, &QAudioOutput::stateChanged, &output,
                         [this, &output](QAudio::State state) {
            if (state == QAudio::StoppedState && output.error() != QAudio::NoError) {
                qWarning() << "AndroidCompatibilityLayer audio: output failed:" << output.error();
                m_queue.setAvailable(false);
            }
        });
        m_queue.setAvailable(true);
        output.start(&source);
        if (output.error() != QAudio::NoError || output.state() == QAudio::StoppedState) {
            qWarning() << "AndroidCompatibilityLayer audio: could not start output:" << output.error();
            m_queue.setAvailable(false);
            return;
        }
        qInfo() << "AndroidCompatibilityLayer audio: Qt output started at" << format.sampleRate() << "Hz";
        QTimer stopTimer;
        QObject::connect(&stopTimer, &QTimer::timeout, &output, [this] {
            if (m_stopping.load()) quit();
        });
        stopTimer.start(20);
        if (!m_stopping.load()) exec();
        m_queue.setAvailable(false);
        output.stop();
    }

private:
    AudioQueue &m_queue;
    int m_rate;
    std::atomic<bool> m_stopping{false};
};

}

class AuroraAudioOutput::Impl
{
public:
    std::mutex mutex;
    std::mutex shutdownMutex;
    bool shutdown = false;
    std::atomic<bool> reportedWrite{false};
    std::atomic<bool> reportedSound{false};
    std::unique_ptr<AudioQueue> queue;
    std::unique_ptr<AudioThread> thread;
};

AuroraAudioOutput::AuroraAudioOutput() : m_impl(new Impl) {}

AuroraAudioOutput::~AuroraAudioOutput()
{
    shutdown();
    delete m_impl;
}

void AuroraAudioOutput::shutdown()
{
    std::lock_guard<std::mutex> shutdownLock(m_impl->shutdownMutex);
    AudioThread *thread;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->shutdown = true;
        if (m_impl->queue) m_impl->queue->close();
        thread = m_impl->thread.get();
        if (thread) thread->stop();
    }
    if (thread) thread->wait();
}

megler_audio_host AuroraAudioOutput::callbacks()
{
    megler_audio_host host = {};
    host.user = this;
    host.open = openCallback;
    host.write = writeCallback;
    return host;
}

bool MEGLER_CB AuroraAudioOutput::openCallback(void *user, int rate)
{
    if (!user || rate < 4000 || rate > 192000) {
        qWarning() << "AndroidCompatibilityLayer audio: invalid output rate:" << rate;
        return false;
    }
    Impl *impl = static_cast<AuroraAudioOutput *>(user)->m_impl;
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->shutdown) {
        qWarning() << "AndroidCompatibilityLayer audio: output has shut down";
        return false;
    }
    if (impl->queue) {
        qWarning() << "AndroidCompatibilityLayer audio: output already opened";
        return false;
    }
    impl->queue.reset(new AudioQueue(rate));
    qInfo() << "AndroidCompatibilityLayer audio: mixer opened output at" << rate << "Hz";
    impl->thread.reset(new AudioThread(*impl->queue, rate));
    impl->thread->start();
    return true;
}

void MEGLER_CB AuroraAudioOutput::writeCallback(void *user, const int16_t *samples, int frames)
{
    if (!user || !samples || frames <= 0 ||
        static_cast<unsigned long long>(frames) > std::numeric_limits<std::size_t>::max() / 4) {
        qWarning() << "AndroidCompatibilityLayer audio: invalid write";
        return;
    }
    Impl *impl = static_cast<AuroraAudioOutput *>(user)->m_impl;
    AudioQueue *queue;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        queue = impl->queue.get();
    }
    if (!queue) {
        qWarning() << "AndroidCompatibilityLayer audio: write before open";
        return;
    }
    if (!impl->reportedWrite.exchange(true))
        qInfo() << "AndroidCompatibilityLayer audio: mixer wrote PCM";
    if (!impl->reportedSound.load()) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2; ++i) {
            if (samples[i] != 0) {
                if (!impl->reportedSound.exchange(true))
                    qInfo() << "AndroidCompatibilityLayer audio: mixer wrote nonzero PCM";
                break;
            }
        }
    }
    if (!queue->write(reinterpret_cast<const char *>(samples), static_cast<std::size_t>(frames) * 4))
        qWarning() << "AndroidCompatibilityLayer audio: output stalled; switching to paced silence";
}
