#ifndef MEGLER_AURORA_AUDIO_FORMAT_H
#define MEGLER_AURORA_AUDIO_FORMAT_H

#include "audio_queue.h"

#include <QAudioFormat>
#include <QList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

inline bool audioConvertible(const QAudioFormat &format)
{
    const int bits = format.sampleSize();
    return format.isValid() && format.codec() == QLatin1String("audio/pcm") &&
           format.sampleRate() >= 4000 && format.sampleRate() <= 384000 &&
           (format.channelCount() == 1 || format.channelCount() == 2) &&
           (format.byteOrder() == QAudioFormat::LittleEndian || format.byteOrder() == QAudioFormat::BigEndian) &&
           ((format.sampleType() == QAudioFormat::Float && (bits == 32 || bits == 64)) ||
            ((format.sampleType() == QAudioFormat::SignedInt || format.sampleType() == QAudioFormat::UnSignedInt) &&
             (bits == 8 || bits == 16 || bits == 24 || bits == 32)));
}

inline QAudioFormat audioInputFormat(int rate)
{
    QAudioFormat format;
    format.setCodec(QLatin1String("audio/pcm"));
    format.setSampleRate(rate);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setSampleType(QAudioFormat::SignedInt);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    format.setByteOrder(QAudioFormat::LittleEndian);
#else
    format.setByteOrder(QAudioFormat::BigEndian);
#endif
    return format;
}

inline QAudioFormat audioChooseFormat(const QAudioFormat &input, const QAudioFormat &nearest,
                                      const QAudioFormat &preferred, QList<int> rates,
                                      const std::function<bool(const QAudioFormat &)> &supported)
{
    const QAudioFormat candidates[] = {input, nearest, preferred};
    for (const QAudioFormat &candidate : candidates)
        if (audioConvertible(candidate) && supported(candidate)) return candidate;
    if (!rates.contains(input.sampleRate())) rates.append(input.sampleRate());
    if (nearest.sampleRate() > 0 && !rates.contains(nearest.sampleRate())) rates.append(nearest.sampleRate());
    if (preferred.sampleRate() > 0 && !rates.contains(preferred.sampleRate())) rates.append(preferred.sampleRate());
    const int standardRates[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 96000, 192000};
    for (int rate : standardRates) if (!rates.contains(rate)) rates.append(rate);
    std::sort(rates.begin(), rates.end(), [&input](int a, int b) {
        return std::abs(static_cast<long long>(a) - input.sampleRate()) <
               std::abs(static_cast<long long>(b) - input.sampleRate());
    });
    const int sizes[] = {16, 32, 24, 8, 64};
    const QAudioFormat::SampleType types[] = {QAudioFormat::SignedInt, QAudioFormat::UnSignedInt, QAudioFormat::Float};
    const int channels[] = {2, 1};
    const QAudioFormat::Endian orders[] = {input.byteOrder(), input.byteOrder() == QAudioFormat::LittleEndian
                                                             ? QAudioFormat::BigEndian : QAudioFormat::LittleEndian};
    for (int rate : rates) {
        if (rate < 4000 || rate > 384000) continue;
        for (int ch : channels) for (int bits : sizes) for (QAudioFormat::SampleType type : types)
            for (QAudioFormat::Endian order : orders) {
                QAudioFormat candidate = input;
                candidate.setSampleRate(rate);
                candidate.setChannelCount(ch);
                candidate.setSampleSize(bits);
                candidate.setSampleType(type);
                candidate.setByteOrder(order);
                if (audioConvertible(candidate) && supported(candidate)) return candidate;
            }
    }
    return QAudioFormat();
}

class AudioConverter
{
public:
    AudioConverter(AudioQueue &queue, int inputRate, const QAudioFormat &format)
        : m_queue(queue), m_inputRate(inputRate), m_format(format), m_phase(format.sampleRate()),
          m_input(static_cast<std::size_t>(1 + (256LL * inputRate + format.sampleRate() - 1) / format.sampleRate()) * 4)
    {
    }

    void read(char *data, std::size_t size, bool &queued, bool &nonzero)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queued = nonzero = false;
        const std::size_t frameBytes = static_cast<std::size_t>(m_format.bytesPerFrame());
        while (size) {
            if (m_offset == m_size) {
                const std::size_t frames = std::min<std::size_t>(256, 1 + (size - 1) / frameBytes);
                generate(frames, queued, nonzero);
            }
            const std::size_t n = std::min(size, m_size - m_offset);
            std::memcpy(data, m_output.data() + m_offset, n);
            data += n;
            size -= n;
            m_offset += n;
        }
    }

private:
    void put(char *out, int16_t sample) const
    {
        const int bits = m_format.sampleSize();
        uint64_t value = 0;
        if (m_format.sampleType() == QAudioFormat::Float) {
            if (bits == 32) {
                const float f = static_cast<float>(sample) / 32768.0f;
                uint32_t raw;
                std::memcpy(&raw, &f, sizeof(raw));
                value = raw;
            } else {
                const double f = static_cast<double>(sample) / 32768.0;
                std::memcpy(&value, &f, sizeof(value));
            }
        } else {
            int64_t scaled = sample;
            if (bits < 16) scaled = ((static_cast<int>(sample) + 32768) >> (16 - bits)) - (1 << (bits - 1));
            else scaled *= int64_t(1) << (bits - 16);
            if (m_format.sampleType() == QAudioFormat::UnSignedInt) scaled += int64_t(1) << (bits - 1);
            value = static_cast<uint64_t>(scaled);
        }
        const int bytes = bits / 8;
        for (int i = 0; i < bytes; ++i) {
            const int shift = 8 * (m_format.byteOrder() == QAudioFormat::LittleEndian ? i : bytes - 1 - i);
            out[i] = static_cast<char>((value >> shift) & 0xff);
        }
    }

    void generate(std::size_t frames, bool &queued, bool &nonzero)
    {
        const int outputRate = m_format.sampleRate();
        const std::size_t count = static_cast<std::size_t>((m_phase + (frames - 1) * static_cast<int64_t>(m_inputRate)) / outputRate);
        const std::size_t bytes = count * 4;
        const std::size_t received = m_queue.read(m_input.data(), bytes);
        queued = queued || received != 0;
        if (!nonzero) for (std::size_t i = 0; i < received; ++i)
            if (m_input[i] != 0) { nonzero = true; break; }
        std::size_t at = 0;
        const int sampleBytes = m_format.sampleSize() / 8;
        const int frameBytes = m_format.bytesPerFrame();
        for (std::size_t i = 0; i < frames; ++i) {
            while (m_phase >= outputRate) {
                std::memcpy(&m_left, m_input.data() + at, sizeof(m_left));
                std::memcpy(&m_right, m_input.data() + at + 2, sizeof(m_right));
                at += 4;
                m_phase -= outputRate;
            }
            char *frame = m_output.data() + i * static_cast<std::size_t>(frameBytes);
            if (m_format.channelCount() == 1)
                put(frame, static_cast<int16_t>((static_cast<int>(m_left) + m_right) / 2));
            else {
                put(frame, m_left);
                put(frame + sampleBytes, m_right);
            }
            m_phase += m_inputRate;
        }
        m_offset = 0;
        m_size = frames * static_cast<std::size_t>(frameBytes);
    }

    AudioQueue &m_queue;
    std::mutex m_mutex;
    const int m_inputRate;
    const QAudioFormat m_format;
    int64_t m_phase;
    int16_t m_left = 0, m_right = 0;
    std::vector<char> m_input;
    std::array<char, 4096> m_output{};
    std::size_t m_offset = 0, m_size = 0;
};

#endif
