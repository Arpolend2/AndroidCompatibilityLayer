#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_METRICS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_METRICS_H

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

enum class PreviewGpuReadbackReason { Flush, Filter, Self, Present };
enum class PreviewGpuFilterFallbackReason { UnsupportedMode, UnsupportedBlend, SelfBitmap, GpuFailure, InvalidBitmap };

class PreviewGpuMetrics : public std::enable_shared_from_this<PreviewGpuMetrics>
{
public:
    struct Sample {
        bool enabled = false;
        bool sampled = false;
        quint64 epoch = 0;
    };

    explicit PreviewGpuMetrics(const std::function<bool()> &enabled = {}) : m_enabled(enabled) { m_clock.start(); }

    Sample beginPresent()
    {
        if (!updateEnabled())
            return {};
        Sample result;
        result.enabled = true;
        result.epoch = m_epoch.load();
        result.sampled = (m_frames.fetch_add(1, std::memory_order_relaxed) & 15u) == 0;
        return result;
    }

    void finishPresent(bool ok, const Sample &sample, quint64 producerUs, quint64 qtWaitUs,
                       bool submittedPixels = false, quint64 pixelSubmitUs = 0)
    {
        if (!sample.enabled || !updateEnabled() || sample.epoch != m_epoch.load())
            return;
        if (!ok)
            m_failed.fetch_add(1, std::memory_order_relaxed);
        if (sample.sampled && m_mutex.tryLock()) {
            resetSamplesFor(sample.epoch);
            ++m_samples;
            m_producerUs += producerUs;
            m_producerMaxUs = qMax(m_producerMaxUs, producerUs);
            m_qtWaitUs += qtWaitUs;
            m_qtWaitMaxUs = qMax(m_qtWaitMaxUs, qtWaitUs);
            if (submittedPixels) {
                ++m_pixelSubmitSamples;
                m_pixelSubmitUs += pixelSubmitUs;
                m_pixelSubmitMaxUs = qMax(m_pixelSubmitMaxUs, pixelSubmitUs);
            }
            m_mutex.unlock();
        }
        maybeReport(sample.epoch);
    }

    Sample beginReadback(PreviewGpuReadbackReason reason)
    {
        if (!updateEnabled())
            return {};
        Sample result;
        result.enabled = true;
        result.epoch = m_epoch.load();
        result.sampled = (m_readbacks.fetch_add(1, std::memory_order_relaxed) & 15u) == 0;
        switch (reason) {
        case PreviewGpuReadbackReason::Flush: m_readbackFlush.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Filter: m_readbackFilter.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Self: m_readbackSelf.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Present: m_readbackPresent.fetch_add(1, std::memory_order_relaxed); break;
        }
        return result;
    }

    void recordReadbackCacheHit(PreviewGpuReadbackReason reason)
    {
        if (!updateEnabled())
            return;
        switch (reason) {
        case PreviewGpuReadbackReason::Flush: m_cacheFlush.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Filter: m_cacheFilter.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Self: m_cacheSelf.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuReadbackReason::Present: m_cachePresent.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    void recordFilterFallback(PreviewGpuFilterFallbackReason reason)
    {
        if (!updateEnabled())
            return;
        switch (reason) {
        case PreviewGpuFilterFallbackReason::UnsupportedMode: m_filterFallbackMode.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuFilterFallbackReason::UnsupportedBlend: m_filterFallbackBlend.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuFilterFallbackReason::SelfBitmap: m_filterFallbackSelf.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuFilterFallbackReason::GpuFailure: m_filterFallbackGpu.fetch_add(1, std::memory_order_relaxed); break;
        case PreviewGpuFilterFallbackReason::InvalidBitmap: m_filterFallbackInvalid.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    void recordFilterDrawError()
    {
        if (updateEnabled())
            m_filterDrawErrors.fetch_add(1, std::memory_order_relaxed);
    }

    void finishReadback(const Sample &sample, quint64 elapsedUs, quint64 glUs, quint64 convertUs)
    {
        if (sample.enabled && sample.sampled && updateEnabled() && sample.epoch == m_epoch.load() && m_mutex.tryLock()) {
            resetSamplesFor(sample.epoch);
            ++m_readbackSamples;
            m_readbackUs += elapsedUs;
            m_readbackMaxUs = qMax(m_readbackMaxUs, elapsedUs);
            m_readbackGlUs += glUs;
            m_readbackConvertUs += convertUs;
            m_mutex.unlock();
        }
    }

    QString takeReport()
    {
        updateEnabled();
        const qint64 now = m_clock.elapsed();
        const quint64 interval = static_cast<quint64>(qMax<qint64>(0, now - m_lastReportMs.exchange(now)));
        const quint64 frames = m_frames.exchange(0);
        const quint64 failed = m_failed.exchange(0);
        const quint64 readbacks = m_readbacks.exchange(0);
        const quint64 readbackFlush = m_readbackFlush.exchange(0);
        const quint64 readbackFilter = m_readbackFilter.exchange(0);
        const quint64 readbackSelf = m_readbackSelf.exchange(0);
        const quint64 readbackPresent = m_readbackPresent.exchange(0);
        const quint64 cacheFlush = m_cacheFlush.exchange(0);
        const quint64 cacheFilter = m_cacheFilter.exchange(0);
        const quint64 cacheSelf = m_cacheSelf.exchange(0);
        const quint64 cachePresent = m_cachePresent.exchange(0);
        const quint64 filterFallbackMode = m_filterFallbackMode.exchange(0);
        const quint64 filterFallbackBlend = m_filterFallbackBlend.exchange(0);
        const quint64 filterFallbackSelf = m_filterFallbackSelf.exchange(0);
        const quint64 filterFallbackGpu = m_filterFallbackGpu.exchange(0);
        const quint64 filterFallbackInvalid = m_filterFallbackInvalid.exchange(0);
        const quint64 filterDrawErrors = m_filterDrawErrors.exchange(0);
        QMutexLocker lock(&m_mutex);
        resetSamplesFor(m_epoch.load());
        const QString result = QStringLiteral(
            "GPU Canvas performance: interval_ms=%1 frames=%2 failed=%3 samples=%4 producer_us=%5 producer_max_us=%6 "
            "qt_wait_us=%7 qt_wait_max_us=%8 readbacks=%9 readback_samples=%10 readback_us=%11 readback_max_us=%12 "
            "readback_flush=%13 readback_filter=%14 readback_self=%15 readback_gl_us=%16 readback_convert_us=%17 "
            "readback_present=%18 pixel_submit_samples=%19 pixel_submit_us=%20 pixel_submit_max_us=%21 "
            "filter_fallback_mode=%22 filter_fallback_blend=%23 filter_fallback_self=%24 filter_fallback_gles=%25 "
            "filter_fallback_invalid=%26 filter_draw_errors=%27 "
            "readback_cache_flush=%28 readback_cache_filter=%29 readback_cache_self=%30 readback_cache_present=%31")
            .arg(interval).arg(frames).arg(failed).arg(m_samples).arg(m_producerUs).arg(m_producerMaxUs)
            .arg(m_qtWaitUs).arg(m_qtWaitMaxUs).arg(readbacks).arg(m_readbackSamples).arg(m_readbackUs).arg(m_readbackMaxUs)
            .arg(readbackFlush).arg(readbackFilter).arg(readbackSelf).arg(m_readbackGlUs).arg(m_readbackConvertUs)
            .arg(readbackPresent).arg(m_pixelSubmitSamples).arg(m_pixelSubmitUs).arg(m_pixelSubmitMaxUs)
            .arg(filterFallbackMode).arg(filterFallbackBlend).arg(filterFallbackSelf).arg(filterFallbackGpu)
            .arg(filterFallbackInvalid).arg(filterDrawErrors)
            .arg(cacheFlush).arg(cacheFilter).arg(cacheSelf).arg(cachePresent);
        m_samples = m_producerUs = m_producerMaxUs = m_qtWaitUs = m_qtWaitMaxUs = 0;
        m_readbackSamples = m_readbackUs = m_readbackMaxUs = 0;
        m_readbackGlUs = m_readbackConvertUs = 0;
        m_pixelSubmitSamples = m_pixelSubmitUs = m_pixelSubmitMaxUs = 0;
        return result;
    }

private:
    class ReportTask : public QRunnable
    {
    public:
        ReportTask(const std::shared_ptr<PreviewGpuMetrics> &metrics, quint64 epoch) : m_metrics(metrics), m_epoch(epoch) {}
        void run() override
        {
            if (m_metrics->updateEnabled() && m_epoch == m_metrics->m_epoch.load()) {
                const QString report = m_metrics->takeReport();
                if (m_metrics->updateEnabled() && m_epoch == m_metrics->m_epoch.load())
                    qInfo("%s", qPrintable(report));
            }
            m_metrics->m_pending.store(false);
        }
    private:
        std::shared_ptr<PreviewGpuMetrics> m_metrics;
        quint64 m_epoch;
    };

    bool updateEnabled()
    {
        const bool enabled = m_enabled && m_enabled();
        if (m_active.exchange(enabled) != enabled) {
            m_epoch.fetch_add(1);
            m_frames.exchange(0);
            m_failed.exchange(0);
            m_readbacks.exchange(0);
            m_readbackFlush.exchange(0);
            m_readbackFilter.exchange(0);
            m_readbackSelf.exchange(0);
            m_readbackPresent.exchange(0);
            m_cacheFlush.exchange(0);
            m_cacheFilter.exchange(0);
            m_cacheSelf.exchange(0);
            m_cachePresent.exchange(0);
            m_filterFallbackMode.exchange(0);
            m_filterFallbackBlend.exchange(0);
            m_filterFallbackSelf.exchange(0);
            m_filterFallbackGpu.exchange(0);
            m_filterFallbackInvalid.exchange(0);
            m_filterDrawErrors.exchange(0);
            m_lastReportMs.store(m_clock.elapsed());
        }
        return enabled;
    }

    void resetSamplesFor(quint64 epoch)
    {
        if (m_sampleEpoch == epoch)
            return;
        m_samples = m_producerUs = m_producerMaxUs = m_qtWaitUs = m_qtWaitMaxUs = 0;
        m_readbackSamples = m_readbackUs = m_readbackMaxUs = 0;
        m_readbackGlUs = m_readbackConvertUs = 0;
        m_pixelSubmitSamples = m_pixelSubmitUs = m_pixelSubmitMaxUs = 0;
        m_sampleEpoch = epoch;
    }

    void maybeReport(quint64 epoch)
    {
        const qint64 now = m_clock.elapsed();
        if (now - m_lastReportMs.load() < 3000 || m_pending.exchange(true))
            return;
        qint64 previous = m_lastAttemptMs.load();
        if (now - previous < 1000 || !m_lastAttemptMs.compare_exchange_strong(previous, now)) {
            m_pending.store(false);
            return;
        }
        ReportTask *task = new ReportTask(shared_from_this(), epoch);
        if (!QThreadPool::globalInstance()->tryStart(task)) {
            delete task;
            m_pending.store(false);
        }
    }

    const std::function<bool()> m_enabled;
    QElapsedTimer m_clock;
    std::atomic<bool> m_active{false};
    std::atomic<quint64> m_epoch{0};
    std::atomic<qint64> m_lastReportMs{0};
    std::atomic<qint64> m_lastAttemptMs{0};
    std::atomic<bool> m_pending{false};
    std::atomic<quint64> m_frames{0}, m_failed{0}, m_readbacks{0};
    std::atomic<quint64> m_readbackFlush{0}, m_readbackFilter{0}, m_readbackSelf{0}, m_readbackPresent{0};
    std::atomic<quint64> m_cacheFlush{0}, m_cacheFilter{0}, m_cacheSelf{0}, m_cachePresent{0};
    std::atomic<quint64> m_filterFallbackMode{0}, m_filterFallbackBlend{0}, m_filterFallbackSelf{0};
    std::atomic<quint64> m_filterFallbackGpu{0}, m_filterFallbackInvalid{0}, m_filterDrawErrors{0};
    QMutex m_mutex;
    quint64 m_sampleEpoch = 0;
    quint64 m_samples = 0, m_producerUs = 0, m_producerMaxUs = 0, m_qtWaitUs = 0, m_qtWaitMaxUs = 0;
    quint64 m_readbackSamples = 0, m_readbackUs = 0, m_readbackMaxUs = 0;
    quint64 m_readbackGlUs = 0, m_readbackConvertUs = 0;
    quint64 m_pixelSubmitSamples = 0, m_pixelSubmitUs = 0, m_pixelSubmitMaxUs = 0;
};

#endif
