#include "install_request_host.h"

#include <QDir>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>

#include <cstring>

InstallRequestHost::InstallRequestHost(const QString &endpoint, const QString &resultsDir,
                                       const megler_system_host &other, QObject *parent)
    : QObject(parent), m_directory(resultsDir), m_client(endpoint, this), m_other(other)
{
    if (m_directory.isEmpty() || !QDir(m_directory).mkdir(QStringLiteral("installs"))) {
        m_error = tr("Cannot create the installation result directory.");
        return;
    }
    QSaveFile marker(m_directory + QStringLiteral("/apps.txt"));
    const QByteArray capability("MEGLER_INSTALL_STATUS_V1\n");
    if (!marker.open(QIODevice::WriteOnly) || marker.write(capability) != capability.size() || !marker.commit()) {
        m_error = tr("Cannot initialize the installation result directory.");
        return;
    }
    connect(&m_client, &InstallRequestClient::completed, this, &InstallRequestHost::completed);
}

QString InstallRequestHost::appsDir() const
{
    return m_error.isEmpty() ? m_directory : QString();
}

megler_system_host InstallRequestHost::host()
{
    megler_system_host host = {};
    host.user = this;
    host.open_url = m_other.open_url ? openUrl : nullptr;
    host.share_text = m_other.share_text ? shareText : nullptr;
    host.install_apk = m_error.isEmpty() ? install : nullptr;
    host.launch_app = m_other.launch_app ? launch : nullptr;
    host.uninstall_app = m_other.uninstall_app ? uninstall : nullptr;
    host.permission = m_other.permission ? permission : nullptr;
    host.pick = m_other.pick ? pick : nullptr;
    return host;
}

void MEGLER_CB InstallRequestHost::openUrl(void *user, const char *uri)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    self->m_other.open_url(self->m_other.user, uri);
}

void MEGLER_CB InstallRequestHost::shareText(void *user, const char *text, const char *subject)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    self->m_other.share_text(self->m_other.user, text, subject);
}

bool MEGLER_CB InstallRequestHost::install(void *user, const char *path, const char *name)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    if (!name) return false;
    const size_t size = strnlen(name, 256);
    if (size < 44 || size > 255) return false;
    const QString text = QString::fromLatin1(name, static_cast<int>(size));
    static const QRegularExpression pattern(QStringLiteral("^request_[0-9a-f]{32}(_[0-9]{1,10}_[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+)?\\.apk$"));
    if (!pattern.match(text).hasMatch()) return false;
    QMutexLocker lock(&self->m_mutex);
    if (!self->m_pendingName.isEmpty() || self->m_resultCount >= 256) return false;
    self->m_pendingName = text;
    if (self->m_client.submit(path, name)) return true;
    self->m_pendingName.clear();
    return false;
}

bool MEGLER_CB InstallRequestHost::launch(void *user, const char *package, const char *activity)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    return self->m_other.launch_app(self->m_other.user, package, activity);
}

bool MEGLER_CB InstallRequestHost::uninstall(void *user, const char *package)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    return self->m_other.uninstall_app(self->m_other.user, package);
}

int MEGLER_CB InstallRequestHost::permission(void *user, const char *group, bool ask)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    return self->m_other.permission(self->m_other.user, group, ask);
}

bool MEGLER_CB InstallRequestHost::pick(void *user, const megler_pick_request *request)
{
    InstallRequestHost *self = static_cast<InstallRequestHost *>(user);
    return self->m_other.pick(self->m_other.user, request);
}

void InstallRequestHost::completed(const QString &outcome, const QString &message,
                                   const QString &package, const QString &label, quint32 version)
{
    QMutexLocker lock(&m_mutex);
    if (m_pendingName.isEmpty()) return;
    ++m_resultCount;
    QString finalOutcome = outcome;
    QString reason = message;
    if (outcome == QLatin1String("installed")) {
        static const QRegularExpression packagePattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
        QString safeLabel = label;
        safeLabel.replace(QLatin1Char('\n'), QLatin1Char(' '));
        safeLabel.replace(QLatin1Char('\r'), QLatin1Char(' '));
        safeLabel.replace(QLatin1Char('\t'), QLatin1Char(' '));
        if (!packagePattern.match(package).hasMatch()) {
            finalOutcome = QStringLiteral("failed");
            reason = tr("Installed package metadata is invalid.");
        } else {
            m_apps.insert(package, package + QLatin1String("\t\t") + safeLabel + QLatin1String("\t\t") + QString::number(version) + QLatin1Char('\n'));
            QByteArray contents("MEGLER_INSTALL_STATUS_V1\n");
            for (const QString &line : m_apps) contents += line.toUtf8();
            QSaveFile apps(m_directory + QStringLiteral("/apps.txt"));
            if (!apps.open(QIODevice::WriteOnly) || apps.write(contents) != contents.size() || !apps.commit()) {
                finalOutcome = QStringLiteral("failed");
                reason = tr("APK installed, but the app list could not be updated.");
                qWarning("Cannot update the installation app list: %s", qPrintable(apps.errorString()));
            }
        }
    }
    reason.replace(QLatin1Char('\n'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\r'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\t'), QLatin1Char(' '));
    QSaveFile result(m_directory + QStringLiteral("/installs/") + m_pendingName + QStringLiteral(".txt"));
    const QByteArray contents = QByteArray("1\t") + finalOutcome.toUtf8() + '\t' + reason.toUtf8();
    if (!result.open(QIODevice::WriteOnly) || result.write(contents) != contents.size() || !result.commit())
        qWarning("Cannot write an installation result: %s", qPrintable(result.errorString()));
    m_pendingName.clear();
}
