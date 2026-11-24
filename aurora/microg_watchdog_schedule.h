#ifndef ACL_MICROG_WATCHDOG_SCHEDULE_H
#define ACL_MICROG_WATCHDOG_SCHEDULE_H

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class MicrogWatchdogSchedule : public QObject
{
public:
    using Completion = std::function<void(bool)>;
    using Operation = std::function<void(bool, const Completion &)>;

    explicit MicrogWatchdogSchedule(const Operation &operation, int retryMilliseconds = 30000)
        : m_operation(operation)
    {
        m_retry.setSingleShot(true);
        m_retry.setInterval(retryMilliseconds);
        QObject::connect(&m_retry, &QTimer::timeout, this, [this]() { apply(); });
    }

    void setEnabled(bool enabled)
    {
        if (!m_requested || m_desired != enabled) {
            m_requested = true;
            m_desired = enabled;
            m_retry.stop();
        }
        apply();
    }

private:
    void apply()
    {
        if (!m_requested || m_pending || m_retry.isActive() ||
            (m_known && m_applied == m_desired)) return;
        const bool requested = m_desired;
        m_pending = true;
        QPointer<MicrogWatchdogSchedule> self(this);
        m_operation(requested, [self, requested](bool success) {
            if (!self) return;
            self->m_pending = false;
            if (success) {
                self->m_known = true;
                self->m_applied = requested;
            } else {
                self->m_known = false;
                if (self->m_desired == requested) self->m_retry.start();
            }
            self->apply();
        });
    }

    Operation m_operation;
    QTimer m_retry;
    bool m_requested = false;
    bool m_desired = false;
    bool m_pending = false;
    bool m_known = false;
    bool m_applied = false;
};

#endif
