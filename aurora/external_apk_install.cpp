#include "external_apk_install.h"

#include "external_apk_path.h"
#include "install_request_commit.h"

#include <QDir>
#include <QFileInfo>

namespace {
const quint64 RequestId = 1;
const quint64 MaxApkBytes = 512u * 1024 * 1024;
}

ExternalApkInstall::ExternalApkInstall(const QString &storeRoot, QObject *parent)
    : QObject(parent), m_storeRoot(storeRoot), m_commit(nullptr), m_active(false), m_prompting(false),
      m_allowUpdate(false), m_updating(false)
{
    connect(&m_prepare, &InstallRequestPrepare::ready, this, &ExternalApkInstall::ready);
    connect(&m_prepare, &InstallRequestPrepare::failed, this, [this](quint64 id, const QString &error) {
        if (id == RequestId && m_active) finish(false, error);
    });
}

ExternalApkInstall::~ExternalApkInstall()
{
    if (m_commit) {
        m_commit->cancel();
        m_commit->wait();
        delete m_commit;
    }
}

bool ExternalApkInstall::start(const QString &path, QString *error, const QString &expectedPackage, bool allowUpdate,
                               bool storeHandoff)
{
    if (error) error->clear();
    if (busy() || m_storeRoot.isEmpty()) {
        if (error) *error = tr("Installation is unavailable or busy.");
        return false;
    }
    if (allowUpdate && expectedPackage.isEmpty()) {
        if (error) *error = tr("An expected application is required for updates.");
        return false;
    }
    QString local;
    if (!externalApkPath(path, &local, error)) return false;
    const QFileInfo file(local);
    if (!m_prepare.start(RequestId, file.absolutePath(), local, QStringLiteral("external.apk"), MaxApkBytes, storeHandoff)) {
        if (error) *error = tr("Installation is unavailable or busy.");
        return false;
    }
    m_expectedPackage = expectedPackage;
    m_allowUpdate = allowUpdate;
    m_updating = false;
    m_active = true;
    return true;
}

bool ExternalApkInstall::busy() const
{
    return m_active || m_commit || m_prepare.busy();
}

void ExternalApkInstall::ready(quint64 id)
{
    if (id != RequestId || !m_active) return;
    const InstallRequestCandidate *candidate = m_prepare.candidate(id);
    if (!candidate) {
        finish(false, tr("The requested APK is unavailable."));
        return;
    }
    if (!m_expectedPackage.isEmpty() && candidate->package() != m_expectedPackage) {
        finish(false, tr("The downloaded APK does not contain the expected application."));
        return;
    }
    m_updating = QFileInfo::exists(QDir(m_storeRoot).filePath(candidate->package()));
    if (m_updating && !m_allowUpdate) {
        finish(false, tr("Updates are not supported."));
        return;
    }
    m_prompting = true;
    emit confirmationRequested(candidate->label(), candidate->package(), candidate->version());
}

void ExternalApkInstall::decision(bool accepted)
{
    if (!m_active || !m_prompting) return;
    m_prompting = false;
    if (!accepted) {
        finish(false, tr("Installation was declined."));
        return;
    }
    const InstallRequestCandidate *candidate = m_prepare.candidate(RequestId);
    if (!candidate) {
        finish(false, tr("The approved APK is unavailable."));
        return;
    }
    m_commit = new InstallRequestCommit(candidate->path(), m_storeRoot, candidate->package(), m_updating);
    connect(m_commit, &QThread::finished, this, &ExternalApkInstall::staged);
    m_commit->start();
}

void ExternalApkInstall::unavailable()
{
    if (m_active && m_prompting) finish(false, tr("The confirmation window is unavailable."));
}

void ExternalApkInstall::staged()
{
    InstallRequestCommit *commit = m_commit;
    m_commit = nullptr;
    const InstallRequestCandidate *candidate = m_prepare.candidate(RequestId);
    const QString package = candidate ? candidate->package() : QString();
    const QByteArray fingerprint = candidate ? candidate->fingerprint() : QByteArray();
    QString error;
    const bool installed = m_active && commit->commit(&error);
    if (!installed && error.isEmpty()) error = commit->error();
    if (!installed && error.isEmpty()) error = tr("Could not add the requested APK to the library.");
    delete commit;
    finish(installed, installed ? (m_updating ? tr("APK updated. Compatibility is not guaranteed.")
                                              : tr("APK added to the library. Compatibility is not guaranteed.")) : error,
           installed ? package : QString(), installed ? fingerprint : QByteArray());
}

void ExternalApkInstall::finish(bool installed, const QString &message, const QString &package, const QByteArray &fingerprint)
{
    m_prepare.cancel(RequestId);
    m_active = false;
    m_prompting = false;
    m_expectedPackage.clear();
    m_allowUpdate = false;
    m_updating = false;
    emit finished(installed, message, package, fingerprint);
}
