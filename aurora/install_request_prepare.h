#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_PREPARE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_PREPARE_H

#include "install_request_candidate.h"
#include <QObject>
#include <memory>

class InstallRequestPrepareThread;

// UI-thread owner of one preparation or prepared candidate. Cancel invalidates
// the token immediately; a canceled worker finishes without emitting ready.
class InstallRequestPrepare : public QObject
{
    Q_OBJECT
public:
    explicit InstallRequestPrepare(QObject *parent = nullptr);
    ~InstallRequestPrepare() override;
    bool start(quint64 id, const QString &senderRoot, const QString &source,
               const QString &name, quint64 maxBytes);
    void cancel(quint64 id);
    const InstallRequestCandidate *candidate(quint64 id) const;
    bool busy() const;

signals:
    void ready(quint64 id);
    void failed(quint64 id, const QString &message);
    void available();

private:
    void workerFinished();
    InstallRequestPrepareThread *m_worker;
    std::unique_ptr<InstallRequestCandidate> m_candidate;
    quint64 m_active;
};

#endif
