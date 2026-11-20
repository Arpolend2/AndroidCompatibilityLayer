#include "install_request_coordinator.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

InstallRequestCoordinator::InstallRequestCoordinator(QProcess *process, const QString &senderRoot,
                                                     const QString &storeRoot, QObject *parent)
    : QObject(parent), m_process(process), m_senderRoot(senderRoot), m_storeRoot(storeRoot),
      m_server(process), m_commit(nullptr), m_active(0), m_commitId(0), m_prompting(false)
{
    connect(&m_server, &InstallRequestServer::requested, this, &InstallRequestCoordinator::request);
    connect(&m_server, &InstallRequestServer::canceled, this, &InstallRequestCoordinator::canceled);
    connect(&m_prepare, &InstallRequestPrepare::ready, this, &InstallRequestCoordinator::ready);
    connect(&m_prepare, &InstallRequestPrepare::failed, this, [this](quint64 id, const QString &error) {
        if (id != m_active) return;
        m_server.finish(id, QStringLiteral("incompatible"), error);
        m_active = 0;
    });
}

InstallRequestCoordinator::~InstallRequestCoordinator()
{
    disconnect(&m_server, nullptr, this, nullptr);
    if (m_commit) {
        m_commit->cancel();
        m_commit->wait();
        delete m_commit;
    }
}

void InstallRequestCoordinator::request(quint64 id, const QString &path, const QString &name)
{
    if (m_active || m_commit || m_storeRoot.isEmpty() ||
        !m_prepare.start(id, m_senderRoot, path, name, 512u * 1024 * 1024)) {
        m_server.finish(id, QStringLiteral("failed"), tr("Installation is unavailable or busy."));
        return;
    }
    m_active = id;
}

void InstallRequestCoordinator::ready(quint64 id)
{
    if (id != m_active) return;
    const InstallRequestCandidate *candidate = m_prepare.candidate(id);
    if (!candidate) return;
    if (QFileInfo::exists(QDir(m_storeRoot).filePath(candidate->package()))) {
        m_server.finish(id, QStringLiteral("incompatible"), tr("Updates are not supported."));
        m_prepare.cancel(id);
        m_active = 0;
        return;
    }
    m_prompting = true;
    emit confirmationRequested(id, candidate->label(), candidate->package(), candidate->version());
}

void InstallRequestCoordinator::decision(quint64 id, bool accepted)
{
    if (id != m_active || !m_prompting) return;
    m_prompting = false;
    if (!accepted) {
        m_server.finish(id, QStringLiteral("declined"), tr("Installation was declined."));
        m_prepare.cancel(id);
        m_active = 0;
        return;
    }
    if (!m_server.approve(id)) {
        m_prepare.cancel(id);
        m_active = 0;
        return;
    }
    const InstallRequestCandidate *candidate = m_prepare.candidate(id);
    if (!candidate) {
        m_server.finish(id, QStringLiteral("failed"), tr("The approved APK is unavailable."));
        m_prepare.cancel(id);
        m_active = 0;
        return;
    }
    m_commit = new InstallRequestCommit(candidate->path(), m_storeRoot, candidate->package());
    m_commitId = id;
    connect(m_commit, &QThread::finished, this, &InstallRequestCoordinator::staged);
    m_commit->start();
}

void InstallRequestCoordinator::unavailable(quint64 id)
{
    if (id != m_active || !m_prompting) return;
    m_prompting = false;
    m_server.finish(id, QStringLiteral("failed"), tr("The confirmation window is unavailable."));
    m_prepare.cancel(id);
    m_active = 0;
}

void InstallRequestCoordinator::canceled(quint64 id)
{
    if (id != m_active) return;
    m_active = 0;
    m_prompting = false;
    emit confirmationCanceled(id);
    if (m_commit) m_commit->cancel();
    else m_prepare.cancel(id);
}

void InstallRequestCoordinator::staged()
{
    InstallRequestCommit *commit = m_commit;
    m_commit = nullptr;
    const quint64 id = m_active;
    const quint64 commitId = m_commitId;
    m_commitId = 0;
    const InstallRequestCandidate *candidate = m_prepare.candidate(id);
    QString error;
    if (id && candidate && m_process->state() == QProcess::Running && commit->commit(&error)) {
        if (!m_server.finish(id, QStringLiteral("installed"), tr("Installed."),
                             candidate->package(), candidate->label(), candidate->version()))
            qWarning("APK installed, but its result could not be delivered to the requesting app.");
        emit installed(candidate->package());
    } else if (id) {
        if (error.isEmpty()) error = commit->error();
        if (error.isEmpty()) error = tr("The requesting app is no longer running.");
        m_server.finish(id, QStringLiteral("failed"), error);
    }
    m_prepare.cancel(commitId);
    m_active = 0;
    delete commit;
}
