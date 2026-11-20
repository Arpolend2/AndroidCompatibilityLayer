#include "application_executables.h"
#include "apk_bundle_install.h"

#include "apk_store.h"
#include "app_storage_lock.h"
#include "install_request_snapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QThread>
#include <QSaveFile>
#include <QTimer>

namespace {
class BundleSnapshotThread : public QThread
{
public:
    QString source, destination, extension, error;
    QByteArray fingerprint;
    void run() override
    {
        InstallRequestSnapshot snapshot;
        const QFileInfo file(source);
        std::string why;
        if (!snapshot.capture(file.absolutePath().toUtf8().toStdString(),
                              source.toUtf8().toStdString(),
                              ("handoff." + extension).toUtf8().toStdString(),
                              512u * 1024 * 1024, &why, true)) {
            error = QString::fromStdString(why);
            return;
        }
        QFile input(QString::fromStdString(snapshot.path()));
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            error = QObject::tr("Cannot prepare the shared APK bundle.");
            return;
        }
        char buffer[65536];
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (;;) {
            const qint64 n = input.read(buffer, sizeof(buffer));
            if (n < 0 || (n > 0 && output.write(buffer, n) != n)) {
                error = QObject::tr("Cannot copy the shared APK bundle.");
                return;
            }
            if (!n) break;
            hash.addData(buffer, static_cast<int>(n));
        }
        if (!output.commit()) error = QObject::tr("Cannot save the shared APK bundle.");
        else fingerprint = hash.result().toHex();
    }
};
}

ApkBundleInstall::ApkBundleInstall(const QString &storeRoot, const QString &androidRoot, int densityDpi,
                                   const QString &languages, QObject *parent)
    : QObject(parent), m_storeRoot(storeRoot), m_androidRoot(androidRoot),
      m_densityDpi(densityDpi > 0 ? densityDpi : 160), m_languages(languages),
      m_temporaryDirectory(nullptr), m_process(nullptr)
{
}

ApkBundleInstall::~ApkBundleInstall()
{
    if (m_snapshotWorker) {
        m_snapshotWorker->wait();
        delete m_snapshotWorker;
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished();
    }
    delete m_temporaryDirectory;
}

bool ApkBundleInstall::start(const QString &path, QString *error, bool storeHandoff)
{
    if (error) error->clear();
    if (busy() || m_storeRoot.isEmpty() || m_androidRoot.isEmpty()) {
        if (error) *error = tr("Bundle installation is unavailable or busy.");
        return false;
    }

    const QFileInfo source(path);
    const QString suffix = source.suffix().toLower();
    if ((storeHandoff && suffix != QLatin1String("apks") && suffix != QLatin1String("xapk")) ||
        (suffix != QLatin1String("apks") && suffix != QLatin1String("xapk"))) {
        if (error) *error = tr("Only .apks and .xapk bundles are supported.");
        return false;
    }
    if (!source.isFile() || !source.isReadable()) {
        if (error) *error = tr("Cannot read the selected APK bundle.");
        return false;
    }
    if (!QDir().mkpath(m_storeRoot)) {
        if (error) *error = tr("Cannot create the app storage directory.");
        return false;
    }

    m_temporaryDirectory = new QTemporaryDir(QDir(m_storeRoot).filePath(QStringLiteral(".bundle-XXXXXX")));
    if (!m_temporaryDirectory->isValid()) {
        delete m_temporaryDirectory;
        m_temporaryDirectory = nullptr;
        if (error) *error = tr("Cannot create temporary storage for the APK bundle.");
        return false;
    }

    const QDir staging(m_temporaryDirectory->path());
    const QString dataPath = staging.filePath(QStringLiteral("data"));
    if (!QDir().mkpath(dataPath)) {
        delete m_temporaryDirectory;
        m_temporaryDirectory = nullptr;
        if (error) *error = tr("Cannot create temporary app data storage.");
        return false;
    }

    if (storeHandoff) {
        m_storeHandoff = true;
        m_storeSourceName = QStringLiteral(".store-source.%1").arg(suffix);
        BundleSnapshotThread *worker = new BundleSnapshotThread;
        worker->source = source.absoluteFilePath();
        worker->extension = suffix;
        worker->destination = staging.filePath(m_storeSourceName);
        m_snapshotWorker = worker;
        connect(worker, &QThread::finished, this, [this, worker]() {
            m_snapshotWorker = nullptr;
            const QString error = worker->error;
            const QString source = worker->destination;
            const QString extension = worker->extension;
            m_fingerprint = worker->fingerprint;
            worker->deleteLater();
            if (!error.isEmpty()) {
                finish(false, error);
                return;
            }
            startImporter(source, extension, true);
        });
        worker->start();
        return true;
    }
    startImporter(source.absoluteFilePath(), suffix, false);
    return true;
}

void ApkBundleInstall::startImporter(const QString &source, const QString &suffix, bool storeHandoff)
{
    const QDir staging(m_temporaryDirectory->path());
    const QString dataPath = staging.filePath(QStringLiteral("data"));
    QStringList arguments;
    const QString mode = storeHandoff && suffix == QLatin1String("xapk")
        ? QStringLiteral("--unpack-store-xapk")
        : storeHandoff ? QStringLiteral("--unpack-store-bundle") : QStringLiteral("--unpack-bundle");
    arguments << mode
              << m_androidRoot
              << dataPath
              << source
              << staging.filePath(QStringLiteral(".bundle-input.%1").arg(suffix))
              << staging.filePath(QStringLiteral("base.apk"))
              << staging.filePath(QStringLiteral("splits"))
              << QString::number(m_densityDpi)
              << m_languages;

    m_process = new QProcess(this);
    connect(m_process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &ApkBundleInstall::processFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart && m_process)
            finish(false, tr("Could not start the APK bundle importer: %1").arg(m_process->errorString()));
    });
    if (storeHandoff) {
        QProcess *process = m_process;
        QTimer::singleShot(10 * 60 * 1000, process, [process]() {
            if (process->state() != QProcess::NotRunning) {
                qWarning("Store APK bundle importer timed out.");
                process->kill();
            }
        });
    }
    m_process->start(ApplicationExecutables::worker(), arguments);
}

void ApkBundleInstall::processFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = m_process;
    if (!process) return;
    const QByteArray output = process->readAllStandardOutput();
    const QString diagnostic = QString::fromUtf8(process->readAllStandardError()).trimmed();
    m_process = nullptr;
    process->deleteLater();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QString detail = diagnostic.isEmpty() ? QString() : tr(" %1").arg(diagnostic);
        finish(false, tr("The APK bundle importer returned an invalid result.%1").arg(detail));
        return;
    }
    const QJsonObject result = document.object();
    if (!result.value(QStringLiteral("ok")).toBool()) {
        QString error = result.value(QStringLiteral("error")).toString();
        if (error.isEmpty()) error = tr("The APK bundle could not be imported.");
        finish(false, error);
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        finish(false, diagnostic.isEmpty() ? tr("The APK bundle importer exited unexpectedly.") : diagnostic);
        return;
    }

    const QString package = result.value(QStringLiteral("package")).toString();
    const ApkStore store(m_storeRoot);
    const QString apk = store.apkPath(package);
    const QDir staging(m_temporaryDirectory->path());
    if (m_storeHandoff && !QFile::remove(staging.filePath(m_storeSourceName))) {
        finish(false, tr("Could not remove the temporary APK bundle copy."));
        return;
    }
    const QFileInfo stagedData(staging.filePath(QStringLiteral("data")));
    const QFileInfo stagedApk(staging.filePath(QStringLiteral("base.apk")));
    const QFileInfo stagedSplits(staging.filePath(QStringLiteral("splits")));
    if (apk.isEmpty() || !stagedApk.isFile() || stagedApk.isSymLink() ||
        !stagedSplits.isDir() || stagedSplits.isSymLink() ||
        !stagedData.isDir() || stagedData.isSymLink()) {
        finish(false, tr("The imported bundle has an invalid app package or is incomplete."));
        return;
    }

    m_package = package;
    m_prompting = true;
    emit confirmationRequested(package, package, result.value(QStringLiteral("versionCode")).toInt());
}

void ApkBundleInstall::decision(bool accepted)
{
    if (!m_prompting || !m_temporaryDirectory) return;
    m_prompting = false;
    if (!accepted) {
        finish(false, tr("Bundle import was declined."));
        return;
    }
    const QString package = m_package;
    const ApkStore store(m_storeRoot);
    const QString apk = store.apkPath(package);
    QLockFile lock(store.lockPath(package));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        finish(false, tr("The app is running or its data is in use."));
        return;
    }
    const QString destination = QFileInfo(apk).absolutePath();
    const QFileInfo destinationInfo(destination);
    if (destinationInfo.exists() || destinationInfo.isSymLink()) {
        finish(false, tr("This app is already installed; updates are not supported yet."));
        return;
    }

    QDir root(m_storeRoot);
    const QString stagingName = QFileInfo(m_temporaryDirectory->path()).fileName();
    if (!root.rename(stagingName, package)) {
        finish(false, tr("Could not safely add the APK bundle to the app library."));
        return;
    }
    delete m_temporaryDirectory;
    m_temporaryDirectory = nullptr;
    finish(true, tr("Added %1 from a split APK bundle to the library. Compatibility is not guaranteed.").arg(package));
}

void ApkBundleInstall::unavailable()
{
    if (m_prompting) finish(false, tr("The confirmation window is unavailable."));
}

void ApkBundleInstall::finish(bool installed, const QString &message)
{
    const QString package = installed ? m_package : QString();
    const QByteArray fingerprint = installed ? m_fingerprint : QByteArray();
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    delete m_temporaryDirectory;
    m_temporaryDirectory = nullptr;
    m_prompting = false;
    m_storeHandoff = false;
    m_storeSourceName.clear();
    m_package.clear();
    m_fingerprint.clear();
    emit finished(installed, message, package, fingerprint);
}
