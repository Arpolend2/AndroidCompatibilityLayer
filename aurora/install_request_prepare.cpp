#include "install_request_prepare.h"

#include <QThread>

class InstallRequestPrepareThread : public QThread
{
public:
    InstallRequestPrepareThread(quint64 id, const QString &root, const QString &source,
                                const QString &name, quint64 maxBytes)
        : id(id), root(root), source(source), name(name), maxBytes(maxBytes) {}

    void run() override
    {
        candidate.reset(new InstallRequestCandidate);
        if (!candidate->prepare(root, source, name, maxBytes, &error)) candidate.reset();
    }

    quint64 id;
    QString root, source, name, error;
    quint64 maxBytes;
    std::unique_ptr<InstallRequestCandidate> candidate;
};

InstallRequestPrepare::InstallRequestPrepare(QObject *parent)
    : QObject(parent), m_worker(nullptr), m_active(0) {}

InstallRequestPrepare::~InstallRequestPrepare()
{
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
}

bool InstallRequestPrepare::start(quint64 id, const QString &senderRoot, const QString &source,
                                  const QString &name, quint64 maxBytes)
{
    if (!id || busy()) return false;
    m_active = id;
    m_worker = new InstallRequestPrepareThread(id, senderRoot, source, name, maxBytes);
    connect(m_worker, &QThread::finished, this, &InstallRequestPrepare::workerFinished);
    m_worker->start();
    return true;
}

void InstallRequestPrepare::cancel(quint64 id)
{
    if (!id || id != m_active) return;
    m_active = 0;
    m_candidate.reset();
    if (!m_worker) emit available();
}

const InstallRequestCandidate *InstallRequestPrepare::candidate(quint64 id) const
{
    return id && id == m_active ? m_candidate.get() : nullptr;
}

bool InstallRequestPrepare::busy() const
{
    return m_worker || m_active;
}

void InstallRequestPrepare::workerFinished()
{
    InstallRequestPrepareThread *worker = m_worker;
    m_worker = nullptr;
    const quint64 id = worker->id;
    const QString error = worker->error;
    if (id == m_active) m_candidate = std::move(worker->candidate);
    worker->deleteLater();
    if (id != m_active) {
        emit available();
    } else if (m_candidate) {
        emit ready(id);
    } else {
        m_active = 0;
        emit failed(id, error);
        emit available();
    }
}
