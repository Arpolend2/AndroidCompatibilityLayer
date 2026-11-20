#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_AUDIO_OUTPUT_RETRY_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_AUDIO_OUTPUT_RETRY_H

namespace aurora_audio_detail {

class RetryBackoff
{
public:
    int nextDelay()
    {
        const int delay = m_delay;
        m_delay = m_delay < maximumDelay / 2 ? m_delay * 2 : maximumDelay;
        return delay;
    }

    void reset() { m_delay = initialDelay; }

private:
    enum { initialDelay = 1000, maximumDelay = 30000 };
    int m_delay = initialDelay;
};

}

#endif
