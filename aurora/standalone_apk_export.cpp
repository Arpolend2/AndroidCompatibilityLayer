#include "standalone_apk_export.h"

#include "install_request_snapshot.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QUuid>

#include <cstring>
#include <QCoreApplication>

namespace {
class ExportThread : public QThread
{
public:
    QString root, source, name, output, error;
    void run() override
    {
        InstallRequestSnapshot snapshot;
        std::string why;
        if (!snapshot.capture(root.toUtf8().toStdString(), source.toUtf8().toStdString(),
                              name.toUtf8().toStdString(), 512u * 1024 * 1024, &why)) {
            error = QString::fromStdString(why);
            return;
        }
        const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QFileInfo folder(downloads);
        if (downloads.isEmpty() || folder.isSymLink() || !folder.isDir() || folder.canonicalFilePath().isEmpty()) {
            error = QCoreApplication::translate("ExportThread", "The Downloads folder is unavailable.");
            return;
        }
        QString id = QUuid::createUuid().toString();
        id.remove(QLatin1Char('{'));
        id.remove(QLatin1Char('}'));
        output = QDir(folder.canonicalFilePath()).filePath(QStringLiteral("Megler-") + id + QStringLiteral(".apk"));
        QFile input(QString::fromStdString(snapshot.path()));
        QSaveFile file(output);
        if (!input.open(QIODevice::ReadOnly) || !file.open(QIODevice::WriteOnly)) {
            error = QCoreApplication::translate("ExportThread", "Could not export the APK to Downloads.");
            output.clear();
            return;
        }
        char buffer[65536];
        for (;;) {
            const qint64 count = input.read(buffer, sizeof(buffer));
            if (count < 0 || (count > 0 && file.write(buffer, count) != count)) {
                error = QCoreApplication::translate("ExportThread", "Could not copy the APK to Downloads.");
                output.clear();
                return;
            }
            if (!count) break;
        }
        if (!file.commit()) {
            error = QCoreApplication::translate("ExportThread", "Could not save the APK in Downloads.");
            output.clear();
        }
    }
};
}

StandaloneApkExport::StandaloneApkExport(const QString &dataRoot, const megler_system_host &other, QObject *parent)
    : QObject(parent), m_dataRoot(dataRoot), m_other(other)
{
    if (!m_directory.isValid() || !QDir(m_directory.path()).mkdir(QStringLiteral("installs"))) {
        m_error = tr("Could not create the APK handoff directory.");
        return;
    }
    QSaveFile marker(m_directory.path() + QStringLiteral("/apps.txt"));
    const QByteArray contents("MEGLER_INSTALL_STATUS_V1\n");
    if (!marker.open(QIODevice::WriteOnly) || marker.write(contents) != contents.size() || !marker.commit()) {
        m_error = tr("Could not initialize the APK handoff.");
        return;
    }
    connect(this, &StandaloneApkExport::requested, this, &StandaloneApkExport::processRequest, Qt::QueuedConnection);
}

QString StandaloneApkExport::appsDir() const { return m_error.isEmpty() ? m_directory.path() : QString(); }

megler_system_host StandaloneApkExport::host()
{
    megler_system_host value = {};
    value.user = this;
    value.open_url = m_other.open_url ? openUrl : nullptr;
    value.share_text = m_other.share_text ? shareText : nullptr;
    value.install_apk = m_error.isEmpty() ? install : nullptr;
    value.launch_app = m_other.launch_app ? launch : nullptr;
    value.uninstall_app = m_other.uninstall_app ? uninstall : nullptr;
    value.permission = m_other.permission ? permission : nullptr;
    value.pick = m_other.pick ? pick : nullptr;
    return value;
}

void MEGLER_CB StandaloneApkExport::openUrl(void *u, const char *uri) { auto s = static_cast<StandaloneApkExport *>(u); s->m_other.open_url(s->m_other.user, uri); }
void MEGLER_CB StandaloneApkExport::shareText(void *u, const char *text, const char *subject) { auto s = static_cast<StandaloneApkExport *>(u); s->m_other.share_text(s->m_other.user, text, subject); }
bool MEGLER_CB StandaloneApkExport::launch(void *u, const char *pkg, const char *activity) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.launch_app(s->m_other.user, pkg, activity); }
bool MEGLER_CB StandaloneApkExport::uninstall(void *u, const char *pkg) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.uninstall_app(s->m_other.user, pkg); }
int MEGLER_CB StandaloneApkExport::permission(void *u, const char *group, bool ask) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.permission(s->m_other.user, group, ask); }
bool MEGLER_CB StandaloneApkExport::pick(void *u, const megler_pick_request *request) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.pick(s->m_other.user, request); }

bool MEGLER_CB StandaloneApkExport::install(void *user, const char *path, const char *name)
{
    auto self = static_cast<StandaloneApkExport *>(user);
    if (!path || !name) return false;
    const size_t pathLength = strnlen(path, 4097), nameLength = strnlen(name, 256);
    if (!pathLength || pathLength > 4096 || nameLength > 255) return false;
    const QString request = QString::fromLatin1(name, static_cast<int>(nameLength));
    static const QRegularExpression pattern(QStringLiteral("^request_[0-9a-f]{32}(_[0-9]{1,10}_[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+)?\\.apk$"));
    if (!pattern.match(request).hasMatch()) return false;
    QMutexLocker lock(&self->m_mutex);
    if (self->m_busy) return false;
    self->m_busy = true;
    emit self->requested(QString::fromUtf8(path, static_cast<int>(pathLength)), request);
    return true;
}

void StandaloneApkExport::processRequest(const QString &path, const QString &name)
{
    ExportThread *worker = new ExportThread;
    worker->root = m_dataRoot;
    worker->source = path;
    worker->name = name;
    connect(worker, &QThread::finished, this, [this, worker, name]() {
        const QString output = worker->output;
        const QString error = worker->error;
        worker->deleteLater();
        if (!error.isEmpty()) reportFailure(name, error);
        else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(output)))
            reportFailure(name, tr("The APK was saved to %1, but could not be opened.").arg(output));
        else
            reportFailure(name, tr("The APK was opened in the system file handler. Complete installation in the manager; its result cannot be reported here."));
        QMutexLocker lock(&m_mutex);
        m_busy = false;
    });
    worker->start();
}

void StandaloneApkExport::reportFailure(const QString &name, const QString &message)
{
    QString reason = message;
    reason.replace(QLatin1Char('\n'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\r'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\t'), QLatin1Char(' '));
    QSaveFile result(m_directory.path() + QStringLiteral("/installs/") + name + QStringLiteral(".txt"));
    const QByteArray contents = QByteArray("1\tfailed\t") + reason.toUtf8();
    if (!result.open(QIODevice::WriteOnly) || result.write(contents) != contents.size() || !result.commit())
        qWarning("Could not report APK handoff failure: %s", qPrintable(result.errorString()));
}
