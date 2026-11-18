#include "apk_bundle_install.h"

#include "apk_store.h"
#include "app_storage_lock.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

ApkBundleInstall::ApkBundleInstall(const QString &storeRoot, const QString &androidRoot, int densityDpi,
                                   const QString &languages, QObject *parent)
    : QObject(parent), m_storeRoot(storeRoot), m_androidRoot(androidRoot),
      m_densityDpi(densityDpi > 0 ? densityDpi : 160), m_languages(languages),
      m_temporaryDirectory(nullptr), m_process(nullptr)
{
}

ApkBundleInstall::~ApkBundleInstall()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished();
    }
    delete m_temporaryDirectory;
}

bool ApkBundleInstall::start(const QString &path, QString *error)
{
    if (error) error->clear();
    if (busy() || m_storeRoot.isEmpty() || m_androidRoot.isEmpty()) {
        if (error) *error = tr("Bundle installation is unavailable or busy.");
        return false;
    }

    const QFileInfo source(path);
    const QString suffix = source.suffix().toLower();
    if (suffix != QLatin1String("apks") && suffix != QLatin1String("xapk")) {
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

    QStringList arguments;
    arguments << QStringLiteral("--unpack-bundle")
              << m_androidRoot
              << dataPath
              << source.absoluteFilePath()
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
    m_process->start(QCoreApplication::applicationFilePath(), arguments);
    return true;
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
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    delete m_temporaryDirectory;
    m_temporaryDirectory = nullptr;
    m_prompting = false;
    m_package.clear();
    emit finished(installed, message);
}
