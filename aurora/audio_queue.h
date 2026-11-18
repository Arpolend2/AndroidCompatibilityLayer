#ifndef MEGLER_AURORA_AUDIO_QUEUE_H
#define MEGLER_AURORA_AUDIO_QUEUE_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

class AudioQueue
{
public:
    explicit AudioQueue(int rate)
        : m_rate(rate), m_data(static_cast<std::size_t>(rate) * 150 / 1000 * 4)
    {
    }

    void setAvailable(bool available)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_available = available && !m_closed;
        if (!m_available) m_head = m_size = 0;
        m_changed.notify_all();
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_available = false;
        m_head = m_size = 0;
        m_changed.notify_all();
    }

    // Returns false if output stalled; later writes are paced and discarded.
    bool write(const char *data, std::size_t size)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool stalled = false;
        while (size && m_available) {
            if (m_size == m_data.size()) {
                if (!m_changed.wait_for(lock, std::chrono::milliseconds(250),
                                        [this] { return !m_available || m_size < m_data.size(); })) {
                    m_available = false;
                    m_head = m_size = 0;
                    stalled = true;
                    m_changed.notify_all();
                    break;
                }
                continue;
            }
            const std::size_t tail = (m_head + m_size) % m_data.size();
            const std::size_t count = std::min(size, std::min(m_data.size() - m_size, m_data.size() - tail));
            std::memcpy(m_data.data() + tail, data, count);
            m_size += count;
            data += count;
            size -= count;
        }
        if (size) {
            const auto duration = std::chrono::nanoseconds(
                static_cast<long long>(size) * 1000000000LL / (static_cast<long long>(m_rate) * 4));
            const auto now = std::chrono::steady_clock::now();
            if (m_deadline < now || m_deadline > now + std::chrono::milliseconds(200))
                m_deadline = now;
            m_deadline += duration;
            const auto deadline = m_deadline;
            lock.unlock();
            std::this_thread::sleep_until(deadline);
        } else {
            m_deadline = std::chrono::steady_clock::time_point();
        }
        return !stalled;
    }

    std::size_t read(char *data, std::size_t size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t count = std::min(size, m_size);
        const std::size_t first = std::min(count, m_data.size() - m_head);
        if (first) std::memcpy(data, m_data.data() + m_head, first);
        if (count > first) std::memcpy(data + first, m_data.data(), count - first);
        m_head = (m_head + count) % m_data.size();
        m_size -= count;
        std::memset(data + count, 0, size - count);
        if (count) m_changed.notify_all();
        return count;
    }

private:
    const int m_rate;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<char> m_data;
    std::size_t m_head = 0;
    std::size_t m_size = 0;
    bool m_available = false;
    bool m_closed = false;
    std::chrono::steady_clock::time_point m_deadline;
};

#endif
