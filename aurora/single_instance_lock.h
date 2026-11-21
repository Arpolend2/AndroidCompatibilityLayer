#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_SINGLE_INSTANCE_LOCK_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_SINGLE_INSTANCE_LOCK_H

#include <QObject>
#include <QString>
#include <QTimer>

// mapplauncherd's booster takes an fcntl lock on
// $XDG_RUNTIME_DIR/single-instance-locks/<binary>/instance.lock inside the process that becomes the
// app and keeps it until exit. While it is held, every --single-instance launch of the same binary
// (icon tap, D-Bus activation) only asks lipstick to raise an existing window. A manager without a
// window must therefore release it so a launch reaches the process and is forwarded over D-Bus.
class SingleInstanceLock : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstanceLock(QObject *parent = nullptr, int retryMilliseconds = 500, int maxRetries = 120);
    ~SingleInstanceLock() override;

    bool adoptInherited();
    bool held() const { return m_fd >= 0; }
    bool retrying() const { return m_retry.isActive(); }
    QString path() const { return m_path; }

public slots:
    // Connect to the manager window's visibleChanged(bool).
    void setHeld(bool held);

private:
    bool tryAcquire();
    void release();

    QString m_path;
    int m_fd = -1;
    bool m_wanted = false;
    int m_lastErrno = 0;
    int m_retries = 0;
    int m_maxRetries;
    QTimer m_retry;
};

#endif
