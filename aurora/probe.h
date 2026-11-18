#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PROBE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PROBE_H

#include <QObject>
#include <QString>
#include <QDebug>
#include <QCloseEvent>
#include <QDir>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUrl>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMap>
#include <QLockFile>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QQuickView>
#include <QScreen>
#include <QUuid>

#include "apk_store.h"
#include "apk_bundle_install.h"
#include "external_apk_install.h"
#include "microg_install.h"
#include "official_store_download.h"
#include "preview_sessions.h"
#include "runtime_settings.h"
#include "log_channel.h"
#include "diagnostic_log.h"
#include "error_log.h"
#include "permission_settings.h"
#include "preview_viewport.h"
#include "../core/src/apk/mg_apk.h"

class Probe : public QObject
{
    Q_OBJECT
    struct PendingInstall {
        QString token, label, package;
        quint32 version;
        bool external;
        bool bundle;
    };
    Q_PROPERTY(QString result READ result NOTIFY resultChanged)
    Q_PROPERTY(QString appResult READ appResult NOTIFY appResultChanged)
    Q_PROPERTY(bool previewRunning READ previewRunning NOTIFY previewRunningChanged)
    Q_PROPERTY(bool externalInstallBusy READ externalInstallBusy NOTIFY externalInstallBusyChanged)
    Q_PROPERTY(QVariantList installedApps READ installedApps NOTIFY installedAppsChanged)
    Q_PROPERTY(QString deviceOperatingSystem READ deviceOperatingSystem CONSTANT)
    Q_PROPERTY(QString deviceCpuArchitecture READ deviceCpuArchitecture CONSTANT)
    Q_PROPERTY(int renderSizeCap READ renderSizeCap NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(bool standaloneRpmBusy READ standaloneRpmBusy NOTIFY standaloneRpmChanged)
    Q_PROPERTY(QString standaloneRpmStatus READ standaloneRpmStatus NOTIFY standaloneRpmChanged)
    Q_PROPERTY(bool standaloneRpmOpened READ standaloneRpmOpened NOTIFY standaloneRpmChanged)
    Q_PROPERTY(QString standaloneRpmPackage READ standaloneRpmPackage NOTIFY standaloneRpmChanged)
    Q_PROPERTY(QString androidLocale READ androidLocale NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString systemAndroidLocale READ systemAndroidLocale CONSTANT)
    Q_PROPERTY(QString androidTheme READ androidTheme NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString androidLocation READ androidLocation NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString googleServicesProvider READ googleServicesProvider NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString googleServicesGmsApkPath READ googleServicesGmsApkPath NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(bool googleServicesBundled READ googleServicesBundled CONSTANT)
    Q_PROPERTY(bool microgBusy READ microgBusy NOTIFY microgChanged)
    Q_PROPERTY(QString microgStatus READ microgStatus NOTIFY microgChanged)
    Q_PROPERTY(bool microgDownloadActive READ microgDownloadActive NOTIFY microgChanged)
    Q_PROPERTY(int microgDownloadProgress READ microgDownloadProgress NOTIFY microgChanged)
    Q_PROPERTY(bool officialStoreBusy READ officialStoreBusy NOTIFY officialStoreChanged)
    Q_PROPERTY(QString officialStoreStatus READ officialStoreStatus NOTIFY officialStoreChanged)
    Q_PROPERTY(bool officialStoreDownloadActive READ officialStoreDownloadActive NOTIFY officialStoreChanged)
    Q_PROPERTY(int officialStoreDownloadProgress READ officialStoreDownloadProgress NOTIFY officialStoreChanged)
    Q_PROPERTY(QString runtimeSettingsError READ runtimeSettingsError NOTIFY runtimeSettingsChanged)
    Q_PROPERTY(QString coreLog READ coreLog NOTIFY coreLogChanged)
    Q_PROPERTY(bool coreLogAvailable READ coreLogAvailable NOTIFY coreLogChanged)
    Q_PROPERTY(bool installConfirmationVisible READ installConfirmationVisible NOTIFY installConfirmationChanged)
    Q_PROPERTY(QString installConfirmationToken READ installConfirmationToken NOTIFY installConfirmationChanged)
    Q_PROPERTY(QString installConfirmationLabel READ installConfirmationLabel NOTIFY installConfirmationChanged)
    Q_PROPERTY(QString installConfirmationPackage READ installConfirmationPackage NOTIFY installConfirmationChanged)
    Q_PROPERTY(quint32 installConfirmationVersion READ installConfirmationVersion NOTIFY installConfirmationChanged)
    Q_PROPERTY(bool installConfirmationIsUpdate READ installConfirmationIsUpdate NOTIFY installConfirmationChanged)
    Q_PROPERTY(bool installConfirmationIsBundle READ installConfirmationIsBundle NOTIFY installConfirmationChanged)

public:
    explicit Probe(const QString &androidRoot, QObject *parent = nullptr, DiagnosticLog *diagnosticLog = nullptr)
        : QObject(parent), m_androidRoot(androidRoot),
          m_store(appStoragePath()), m_externalInstall(appStoragePath()),
          m_bundleInstall(appStoragePath(), androidRoot, bundleDensityDpi(), RuntimeSettings::locale()),
          m_microgInstall(microgApkPath()),
          m_diagnosticLog(diagnosticLog)
    {
        connect(this, &Probe::installedAppsChanged, this, [this]() { m_installedAppsValid = false; });
        m_standaloneTimeout.setSingleShot(true);
        connect(&m_standaloneTimeout, &QTimer::timeout, this, [this]() {
            if (m_standaloneRpm.state() == QProcess::NotRunning) return;
            m_standaloneTimedOut = true;
            m_standaloneRpm.kill();
        });
        connect(&m_standaloneRpm, &QProcess::readyReadStandardError, this, [this]() {
            readStandaloneRpmDiagnostics(false);
        });
        connect(&m_standaloneRpm, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus status) {
            m_standaloneTimeout.stop();
            const QByteArray output = m_standaloneRpm.readAllStandardOutput();
            readStandaloneRpmDiagnostics(true);
            const QString diagnostic = QString::fromUtf8(m_standaloneDiagnostic).trimmed();
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(output.trimmed(), &parseError);
            const QJsonObject result = doc.object();
            const QString path = result.value(QStringLiteral("path")).toString();
            const QString apk = m_store.apkPath(m_standalonePackage);
            const QString root = QDir(QFileInfo(apk).absolutePath()).filePath(QStringLiteral("standalone-rpm"));
            const QFileInfo file(path);
            if (status != QProcess::NormalExit || exitCode != 0 ||
                parseError.error != QJsonParseError::NoError || !doc.isObject() ||
                apk.isEmpty() || !result.value(QStringLiteral("ok")).toBool() ||
                QFileInfo(root).isSymLink() || !file.isFile() || file.isSymLink() ||
                QDir::cleanPath(file.absolutePath()) != QDir::cleanPath(root)) {
                QString message = m_standaloneTimedOut ? tr("The RPM builder timed out.")
                                                       : result.value(QStringLiteral("error")).toString();
                if (message.isEmpty()) message = diagnostic;
                if (message.isEmpty()) message = tr("The RPM builder exited unexpectedly.");
                m_standaloneRpmStatus = tr("Could not create the standalone RPM: %1").arg(message);
                ErrorLog::record(QStringLiteral("RPM"), m_standaloneRpmStatus, true);
            } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                m_standaloneRpmStatus = tr("RPM created at %1, but the installer could not be opened.").arg(path);
                ErrorLog::record(QStringLiteral("RPM"), m_standaloneRpmStatus, true);
            } else {
                m_standaloneRpmStatus = tr("RPM created. Confirm installation in the system installer.");
                m_standaloneRpmOpened = true;
            }
            emit standaloneRpmChanged();
            if (!m_standaloneRpmOpened)
                emit standaloneRpmError(m_standalonePackage, m_standaloneRpmStatus);
        });
        connect(&m_standaloneRpm, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            m_standaloneTimeout.stop();
            m_standaloneRpmStatus = tr("Could not start the RPM builder: %1").arg(m_standaloneRpm.errorString());
            ErrorLog::record(QStringLiteral("RPM"), m_standaloneRpmStatus, true);
            emit standaloneRpmChanged();
            emit standaloneRpmError(m_standalonePackage, m_standaloneRpmStatus);
        });
        connect(&m_bundleInstall, &ApkBundleInstall::confirmationRequested, this,
                [this](const QString &, const QString &, quint32) {
            m_bundleInstall.decision(true);
        });
        connect(&m_bundleInstall, &ApkBundleInstall::finished, this, [this](bool installed, const QString &message) {
            m_result = message;
            if (installed) {
                emit installedAppsChanged();
                showSignatureReminder();
            } else qWarning().noquote() << "APK bundle:" << message;
            emit resultChanged();
            emit externalInstallBusyChanged();
            emit officialStoreChanged();
        });
        connect(&m_officialStoreDownload, &OfficialStoreDownload::ready, this, [this](const QString &path) {
            QString error;
            if (!m_externalInstall.start(path, &error, QStringLiteral("com.aurora.store"), true)) {
                finishOfficialStore(false, error);
                return;
            }
            m_officialStoreStatus = tr("Inspecting the Aurora Store APK...");
            m_officialStoreProgress = -1;
            emit officialStoreChanged();
            emit officialStoreDownloadNotification(m_officialStoreStatus, -1, false);
            emit externalInstallBusyChanged();
        });
        connect(&m_officialStoreDownload, &OfficialStoreDownload::downloadProgress, this, [this](int percent) {
            m_officialStoreProgress = percent;
            m_officialStoreStatus = tr("Downloading the Aurora Store APK from GitLab...");
            emit officialStoreChanged();
            emit officialStoreDownloadNotification(m_officialStoreStatus, percent, false);
        });
        connect(&m_officialStoreDownload, &OfficialStoreDownload::failed, this,
                [this](const QString &error) { finishOfficialStore(false, error); });
        connect(&m_microgInstall, &MicrogInstall::downloadProgress, this, [this](int percent) {
            m_microgProgress = percent;
            m_microgStatus = tr("Downloading the microG APK from GitHub...");
            emit microgChanged();
            emit microgDownloadNotification(m_microgStatus, percent, false);
        });
        connect(&m_microgInstall, &MicrogInstall::validatingDownload, this, [this]() {
            m_microgProgress = -1;
            m_microgStatus = tr("Checking and installing the microG APK...");
            emit microgChanged();
            emit microgDownloadNotification(m_microgStatus, -1, false);
        });
        connect(&m_microgInstall, &MicrogInstall::finished, this, [this](bool installed, const QString &message) {
            const bool open = m_microgOpenAfter;
            m_microgOpenAfter = false;
            m_microgStatus = message;
            if (installed) {
                if (!RuntimeSettings::save(QStringLiteral("googleServices/gmsApkPath"), microgApkPath())) {
                    m_microgStatus = tr("microG was downloaded, but its path could not be saved.");
                } else if (open) {
                    if (RuntimeSettings::save(QStringLiteral("googleServices/provider"), QStringLiteral("microg")))
                        launchMicrog(microgApkPath());
                    else
                        m_microgStatus = tr("microG was downloaded, but could not be enabled.");
                }
                emit runtimeSettingsChanged();
            }
            if (m_microgOnline) {
                m_microgOnline = false;
                emit microgDownloadNotification(m_microgStatus, -1, true);
            }
            emit microgChanged();
        });
        if (m_diagnosticLog && m_diagnosticLog->available()) {
            m_sessions.setDiagnosticLog(m_diagnosticLog);
            connect(m_diagnosticLog, &DiagnosticLog::changed, this, &Probe::coreLogChanged);
        }
        connect(&m_externalInstall, &ExternalApkInstall::confirmationRequested, this,
                [this](const QString &label, const QString &package, quint32 version) {
            if (!m_officialStoreActive || m_officialStoreOnline || !m_externalInstall.isUpdate()) {
                if (m_officialStoreActive) {
                    m_officialStoreStatus = tr("Adding the Aurora Store APK to the library...");
                    emit officialStoreChanged();
                    if (m_officialStoreOnline)
                        emit officialStoreDownloadNotification(m_officialStoreStatus, -1, false);
                }
                m_externalInstall.decision(true);
                return;
            }
            enqueueConfirmation(QUuid::createUuid().toString(), label, package, version, true);
        });
        connect(&m_externalInstall, &ExternalApkInstall::finished, this,
                [this](bool installed, const QString &message) {
            if (m_officialStoreActive) {
                if (installed) emit installedAppsChanged();
                finishOfficialStore(installed, message);
                emit externalInstallBusyChanged();
                return;
            }
            if (installed) {
                m_result = message;
                emit installedAppsChanged();
                emit resultChanged();
                showSignatureReminder();
            } else {
                externalInstallError(message);
            }
            emit externalInstallBusyChanged();
        });
        connect(&m_sessions, &PreviewSessions::runningChanged, this, [this]() {
            emit previewRunningChanged();
        });
        connect(&m_sessions, &PreviewSessions::installed, this, &Probe::installedAppsChanged);
        connect(&m_sessions, &PreviewSessions::activationFinished, this,
                [this](const QString &, const QString &key, bool activated, const QString &error) {
            if (activated) {
                m_appResult = tr("%1 is open.").arg(key);
            } else
                m_appResult = tr("Could not switch to %1: %2. Use the system app switcher.").arg(key, error);
            emit appResultChanged();
        });
        connect(&m_sessions, &PreviewSessions::logUnavailable, this, [this](const QString &key) {
            m_logFailed = true;
            qWarning().noquote() << tr("In-app log unavailable for %1; output remains in the IDE.").arg(key);
            emit coreLogChanged();
        });
        connect(&m_sessions, &PreviewSessions::logChanged, this,
                [this](const QString &instance, const QString &key, const QString &text) {
            if (!m_logTexts.contains(instance)) {
                if (m_logOrder.size() >= 16) {
                    const QString oldest = m_logOrder.takeFirst();
                    m_logTexts.remove(oldest);
                    m_logKeys.remove(oldest);
                }
                m_logOrder.append(instance);
                m_logKeys.insert(instance, key);
            }
            m_logTexts.insert(instance, text);
            emit coreLogChanged();
        });
        connect(&m_sessions, &PreviewSessions::confirmationRequested, this,
                [this](const QString &token, const QString &label, const QString &package, quint32 version) {
            enqueueConfirmation(token, label, package, version, false);
        });
        connect(&m_sessions, &PreviewSessions::confirmationCanceled, this, [this](const QString &token) {
            if (m_installToken == token && !m_externalPrompt) {
                clearConfirmation();
            } else {
                for (int i = m_pendingInstalls.size() - 1; i >= 0; --i)
                    if (m_pendingInstalls.at(i).token == token) m_pendingInstalls.removeAt(i);
            }
        });
        connect(&m_sessions, &PreviewSessions::finished, this,
                [this](const QString &, const QString &key, int code, bool crashed, const QString &error) {
            if (m_forcedClosing.remove(key))
                m_appResult = tr("%1 was force closed.").arg(key);
            else if (!error.isEmpty())
                m_appResult = tr("Could not start %1: %2").arg(key, error);
            else if (!crashed && code == 0)
                m_appResult = tr("%1 closed.").arg(key);
            else
                m_appResult = tr("%1 failed (exit code %2). See the application log.").arg(key).arg(code);
            if (key == QLatin1String("__microg__")) {
                m_microgStatus = error.isEmpty()
                    ? (crashed || code != 0 ? tr("microG failed to start or exited with an error. See the app log.")
                                            : tr("microG closed."))
                    : tr("Could not start microG: %1").arg(error);
                emit microgChanged();
            }
            emit appResultChanged();
        });
    }
    QString result() const { return m_result; }
    QString appResult() const { return m_appResult; }
    bool previewRunning() const { return m_sessions.count() != 0; }
    Q_INVOKABLE bool isAppRunning(const QString &package) const { return m_sessions.running(package); }
    bool externalInstallBusy() const { return m_externalInstall.busy() || m_bundleInstall.busy(); }
    QVariantList installedApps() const {
        if (!m_installedAppsValid) {
            m_cachedInstalledApps = m_store.apps();
            m_installedAppsValid = true;
        }
        return m_cachedInstalledApps;
    }
    QString deviceOperatingSystem() const { return QSysInfo::prettyProductName(); }
    QString deviceCpuArchitecture() const { return QSysInfo::currentCpuArchitecture(); }
    int renderSizeCap() const { return RuntimeSettings::renderCap(); }
    bool standaloneRpmBusy() const { return m_standaloneRpm.state() != QProcess::NotRunning; }
    QString standaloneRpmStatus() const { return m_standaloneRpmStatus; }
    bool standaloneRpmOpened() const { return m_standaloneRpmOpened; }
    QString standaloneRpmPackage() const { return m_standalonePackage; }
    Q_INVOKABLE QVariantList appPermissionGroups(const QString &package, const QVariantMap &app)
    {
        m_appResult.clear();
        emit appResultChanged();
        const QString apk = m_store.apkPath(package);
        if (apk.isEmpty() || !QFileInfo(apk).isFile()) {
            m_appResult = tr("The saved APK was not found.");
            emit appResultChanged();
            return QVariantList();
        }
        QStringList denied;
        QString error;
        if (!PermissionSettings::read(QFileInfo(apk).absolutePath(), &denied, &error)) {
            m_appResult = error;
            emit appResultChanged();
            return QVariantList();
        }
        if (app.value(QStringLiteral("package")).toString() != package ||
            !app.contains(QStringLiteral("permissions")) || !app.contains(QStringLiteral("permissionMaxSdk"))) {
            m_appResult = tr("Could not inspect the saved APK permissions.");
            emit appResultChanged();
            return QVariantList();
        }
        return PermissionSettings::requested(app.value(QStringLiteral("permissions")).toList(),
                                             app.value(QStringLiteral("permissionMaxSdk")).toList(), denied);
    }

    Q_INVOKABLE bool setAppPermission(const QString &package, const QString &group, bool allowed)
    {
        const QString apk = m_store.apkPath(package);
        const QString lockPath = m_store.lockPath(package);
        if (standaloneRpmBusy() || apk.isEmpty() || lockPath.isEmpty() ||
            !QFileInfo(apk).isFile() || !PermissionSettings::known(group)) {
            m_appResult = tr("Cannot change the app's permission settings.");
            emit appResultChanged();
            return false;
        }
        QLockFile lock(lockPath);
        lock.setStaleLockTime(0);
        QStringList denied;
        QString error;
        const QString directory = QFileInfo(apk).absolutePath();
        bool ok = lock.tryLock(0);
        if (!ok) error = tr("The app data is in use.");
        if (ok) ok = PermissionSettings::read(directory, &denied, &error);
        if (ok) {
            denied.removeAll(group);
            if (!allowed) denied.append(group);
            ok = PermissionSettings::save(directory, denied, &error);
        }
        m_appResult = ok ? QString() : error;
        emit appResultChanged();
        return ok;
    }
    Q_INVOKABLE void installStandaloneRpm(const QString &package)
    {
        if (standaloneRpmBusy()) return;
        m_standaloneRpmOpened = false;
        m_standalonePackage = package;
        m_standaloneErrorBuffer.clear();
        m_standaloneDiagnostic.clear();
        bool preflightFailed = true;
        if (m_sessions.running(package)) {
            m_standaloneRpmStatus = tr("Close the Android app before creating its RPM.");
        } else if (m_store.apkPath(package).isEmpty() || !QFileInfo(m_store.apkPath(package)).isFile()) {
            m_standaloneRpmStatus = tr("The installed APK was not found.");
        } else {
            preflightFailed = false;
            m_standaloneTimedOut = false;
            m_standaloneRpmStatus = tr("Preparing to create the RPM...");
            m_standaloneRpm.start(QCoreApplication::applicationFilePath(),
                                  QStringList() << QStringLiteral("--build-standalone") << package);
            m_standaloneTimeout.start(15 * 60 * 1000);
        }
        emit standaloneRpmChanged();
        if (preflightFailed)
            emit standaloneRpmError(m_standalonePackage, m_standaloneRpmStatus);
    }
    QString androidLocale() const { return RuntimeSettings::selectedLocale(); }
    QString systemAndroidLocale() const { return RuntimeSettings::systemLocaleTag(QLocale::system()); }
    QString androidTheme() const { return RuntimeSettings::theme(); }
    QString androidLocation() const { return RuntimeSettings::location(); }
    QString googleServicesProvider() const { return QSettings().value(QStringLiteral("googleServices/provider"), QStringLiteral("none")).toString(); }
    QString googleServicesGmsApkPath() const { return QSettings().value(QStringLiteral("googleServices/gmsApkPath")).toString(); }
    bool microgBusy() const { return m_microgInstall.busy(); }
    QString microgStatus() const { return m_microgStatus; }
    bool microgDownloadActive() const { return m_microgOnline && microgBusy(); }
    int microgDownloadProgress() const { return m_microgProgress; }
    bool officialStoreBusy() const { return m_officialStoreActive || m_bundleInstall.busy(); }
    QString officialStoreStatus() const { return m_officialStoreStatus; }
    bool officialStoreDownloadActive() const { return m_officialStoreActive && m_officialStoreOnline; }
    int officialStoreDownloadProgress() const { return m_officialStoreProgress; }
    bool googleServicesBundled() const {
        const QFileInfo file(QDir(m_androidRoot).filePath(QStringLiteral("services/GmsCore.apk")));
        return file.isFile() && file.isReadable();
    }
    QString runtimeSettingsError() const { return m_runtimeSettingsError; }
    QString coreLog() const
    {
        if (m_diagnosticLog) return m_diagnosticLog->text();
        QString result;
        const int budget = m_logOrder.isEmpty() ? 0 : 125000 / m_logOrder.size();
        for (const QString &instance : m_logOrder) {
            result += QStringLiteral("[%1 %2]\n").arg(m_logKeys.value(instance), instance);
            result += m_logTexts.value(instance).right(budget);
            result += QLatin1Char('\n');
        }
        return result;
    }
    bool coreLogAvailable() const {
        return m_diagnosticLog ? m_diagnosticLog->available() : m_logChannel.available() && !m_logFailed;
    }
    Q_INVOKABLE QString saveCoreLog()
    {
        const QString log = coreLog();
        if (!coreLogAvailable() || log.isEmpty())
            return tr("The log is empty or unavailable.");
        QString path, error;
        if (!ErrorLog::saveText(log, &path, &error))
            return tr("Could not save the log: %1").arg(error);
        return tr("Log saved to %1.").arg(path);
    }
    Q_INVOKABLE void setRenderSizeCap(int value)
    {
        if (!RuntimeSettings::validRenderCap(value)) {
            m_runtimeSettingsError = tr("Invalid render size.");
        } else if (!RuntimeSettings::save(QStringLiteral("runtime/renderCap"), value)) {
            m_runtimeSettingsError = tr("Could not save the render size.");
        } else {
            m_runtimeSettingsError.clear();
        }
        emit runtimeSettingsChanged();
    }
    Q_INVOKABLE void setAndroidLocale(const QString &input)
    {
        QString value = input.trimmed();
        value.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (!value.isEmpty() && !RuntimeSettings::validLocale(value)) {
            m_runtimeSettingsError = tr("Enter a language tag such as en-US or de-DE.");
        } else if (!RuntimeSettings::save(QStringLiteral("runtime/locale"), value)) {
            m_runtimeSettingsError = tr("Could not save the language and region.");
        } else {
            m_runtimeSettingsError.clear();
        }
        emit runtimeSettingsChanged();
    }
    Q_INVOKABLE void setAndroidTheme(const QString &value)
    {
        if (!RuntimeSettings::validTheme(value))
            m_runtimeSettingsError = tr("Invalid theme.");
        else if (!RuntimeSettings::save(QStringLiteral("runtime/theme"), value))
            m_runtimeSettingsError = tr("Could not save the theme.");
        else
            m_runtimeSettingsError.clear();
        emit runtimeSettingsChanged();
    }
    Q_INVOKABLE bool validAndroidLocation(const QString &input) const
    {
        return RuntimeSettings::parseLocation(input.trimmed(), nullptr, nullptr);
    }
    Q_INVOKABLE void setAndroidLocation(const QString &input)
    {
        const QString value = input.trimmed();
        if (!value.isEmpty() && !RuntimeSettings::parseLocation(value, nullptr, nullptr))
            m_runtimeSettingsError = tr("Enter a valid latitude and longitude.");
        else if (!RuntimeSettings::save(QStringLiteral("runtime/location"), value))
            m_runtimeSettingsError = tr("Could not save the location.");
        else
            m_runtimeSettingsError.clear();
        emit runtimeSettingsChanged();
    }
    Q_INVOKABLE void setGoogleServicesProvider(const QString &value)
    {
        if (value != QLatin1String("none") && value != QLatin1String("microg")) {
            m_runtimeSettingsError = tr("Invalid Google services provider.");
            emit runtimeSettingsChanged();
            return;
        }
        if (value == QLatin1String("none")) m_microgOpenAfter = false;
        saveGoogleSetting(QStringLiteral("googleServices/provider"), value);
    }
    Q_INVOKABLE void setGoogleServicesGmsApkPath(const QString &value)
    {
        saveGoogleSetting(QStringLiteral("googleServices/gmsApkPath"), value);
    }
    Q_INVOKABLE void openMicrog()
    {
        if (microgBusy()) return;
        QString path = googleServicesGmsApkPath();
        if (path.isEmpty() && QFileInfo(microgApkPath()).isFile()) path = microgApkPath();
        if (path.isEmpty() && googleServicesBundled())
            path = QDir(m_androidRoot).filePath(QStringLiteral("services/GmsCore.apk"));
        if (path.isEmpty()) {
            startMicrogUpdate(QString(), true);
            return;
        }
        mg_apk_info info = {};
        const QByteArray apk = path.toUtf8();
        if (!QFileInfo(path).isFile()) {
            m_microgStatus = tr("The selected microG APK is missing. Update microG or choose a local APK.");
        } else if (!mg_apk_read_info(apk.constData(), &info) || !mg_apk_gms_name(info.package) ||
                   !mg_apk_is_microg(apk.constData()) || info.split[0] || info.config_for_split[0] ||
                   info.feature_split || info.split_required || info.required_split_types[0]) {
            m_microgStatus = tr("The selected APK is not a supported standalone microG package.");
        } else if (googleServicesGmsApkPath().isEmpty() && path == microgApkPath() &&
                   !RuntimeSettings::save(QStringLiteral("googleServices/gmsApkPath"), path)) {
            m_microgStatus = tr("Could not save the microG APK path.");
        } else if (!RuntimeSettings::save(QStringLiteral("googleServices/provider"), QStringLiteral("microg"))) {
            m_microgStatus = tr("Could not enable microG.");
        } else {
            launchMicrog(path);
        }
        emit runtimeSettingsChanged();
        emit microgChanged();
    }
    Q_INVOKABLE void updateMicrog(const QString &path) { startMicrogUpdate(path, false); }
    Q_INVOKABLE void openOfficialStore()
    {
        if (officialStoreBusy()) return;
        const QString package = QStringLiteral("com.aurora.store");
        if (QFileInfo(m_store.apkPath(package)).isFile()) {
            m_officialStoreStatus = tr("Install Aurora Store as a standalone RPM from the app library.");
            emit officialStoreChanged();
            return;
        }
        startOfficialStore(QString(), true);
    }
    Q_INVOKABLE void updateOfficialStore() { startOfficialStore(QString(), false); }
    Q_INVOKABLE void updateOfficialStoreFromLocal(const QString &path) { startOfficialStore(path, false); }
    bool installConfirmationVisible() const { return m_installVisible; }
    QString installConfirmationToken() const { return m_installToken; }
    QString installConfirmationLabel() const { return m_installLabel; }
    QString installConfirmationPackage() const { return m_installPackage; }
    quint32 installConfirmationVersion() const { return m_installVersion; }
    bool installConfirmationIsUpdate() const { return m_externalPrompt && m_externalInstall.isUpdate(); }
    bool installConfirmationIsBundle() const { return m_bundlePrompt; }

    void setConfirmationWindow(QQuickView *view)
    {
        m_confirmationWindow = view;
        view->installEventFilter(this);
        connect(view, &QWindow::activeChanged, this, [this]() {
            if (m_installVisible && m_confirmationWindow && !m_confirmationWindow->isActive())
                unavailableConfirmation();
        });
    }

    Q_INVOKABLE void answerInstall(const QString &token, bool accepted)
    {
        if (!m_installVisible || token != m_installToken) return;
        if (!m_confirmationWindow || !m_confirmationWindow->isActive() || !m_confirmationWindow->isVisible()) {
            unavailableConfirmation();
            return;
        }
        const bool external = m_externalPrompt;
        const bool bundle = m_bundlePrompt;
        clearConfirmation();
        if (bundle) m_bundleInstall.decision(accepted);
        else if (external) m_externalInstall.decision(accepted);
        else m_sessions.decision(token, accepted);
    }

    Q_INVOKABLE void setInstallUiReady(bool ready)
    {
        m_installUiReady = ready;
        if (!ready && !m_installToken.isEmpty()) unavailableConfirmation();
    }

    void externalInstallError(const QString &message)
    {
        m_result = message;
        qWarning().noquote() << "External APK:" << message;
        emit resultChanged();
        emit externalInstallNotice();
        if (m_confirmationWindow && m_installUiReady) {
            m_confirmationWindow->show();
            m_confirmationWindow->raise();
            m_confirmationWindow->requestActivate();
        }
    }

    void requestExternalInstall(const QString &path)
    {
        QString error;
        if (m_externalInstall.busy() || officialStoreBusy()) {
            externalInstallError(tr("Another installation is in progress."));
            return;
        }
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QLatin1String("apks") || suffix == QLatin1String("xapk")) {
            installApk(path);
            return;
        }
        if (!m_externalInstall.start(path, &error)) {
            externalInstallError(error);
            return;
        }
        m_result = tr("Inspecting the selected APK...");
        emit resultChanged();
        emit externalInstallBusyChanged();
    }

    Q_INVOKABLE void installApk(const QString &path)
    {
        if (m_externalInstall.busy() || m_bundleInstall.busy() || officialStoreBusy()) {
            externalInstallError(tr("Another installation is in progress."));
            return;
        }
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QLatin1String("apks") || suffix == QLatin1String("xapk")) {
            QString error;
            m_bundleInstall.setLanguages(RuntimeSettings::locale());
            if (!m_bundleInstall.start(path, &error)) {
                externalInstallError(error);
                return;
            }
            m_result = tr("Importing the split APK bundle...");
            emit resultChanged();
            emit externalInstallBusyChanged();
            emit officialStoreChanged();
            return;
        }
        if (m_store.install(path, &m_result)) {
            emit installedAppsChanged();
            showSignatureReminder();
        }
        emit resultChanged();
    }

    Q_INVOKABLE bool removeApp(const QString &package)
    {
        if (standaloneRpmBusy()) {
            m_appResult = tr("Wait for RPM creation to finish before removing an app.");
            emit appResultChanged();
            return false;
        }
        if (m_sessions.running(package)) {
            m_appResult = tr("Close the app before removing it.");
            emit appResultChanged();
            return false;
        }
        const bool removed = m_store.remove(package, &m_appResult);
        emit installedAppsChanged();
        emit appResultChanged();
        return removed;
    }

    Q_INVOKABLE void forceCloseApp(const QString &package)
    {
        m_forcedClosing.insert(package);
        m_appResult = tr("Stopping %1...").arg(package);
        if (!m_sessions.kill(package)) {
            m_forcedClosing.remove(package);
            m_appResult = tr("%1 is not running.").arg(package);
        }
        emit appResultChanged();
    }

    Q_INVOKABLE void runInstalled(const QString &package)
    {
        m_appResult = tr("Apps cannot be started from the manager. Install %1 as a standalone RPM.").arg(package);
        qWarning().noquote() << m_appResult;
        emit appResultChanged();
    }

signals:
    void standaloneRpmChanged();
    void standaloneRpmError(const QString &appPackage, const QString &message);
    void signatureReminder();
    void runtimeSettingsChanged();
    void coreLogChanged();
    void resultChanged();
    void appResultChanged();
    void previewRunningChanged();
    void externalInstallBusyChanged();
    void externalInstallNotice();
    void controllerCloseBlocked();
    void installedAppsChanged();
    void microgChanged();
    void officialStoreChanged();
    void microgDownloadNotification(const QString &message, int percent, bool complete);
    void officialStoreDownloadNotification(const QString &message, int percent, bool complete);
    void installConfirmationChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_confirmationWindow && event->type() == QEvent::Close && previewRunning()) {
            static_cast<QCloseEvent *>(event)->ignore();
            m_appResult = tr("Close the running Android apps before closing the compatibility layer.");
            emit appResultChanged();
            emit controllerCloseBlocked();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void showSignatureReminder()
    {
        QSettings settings;
        const QString key = QStringLiteral("library/signatureRemindersShown");
        const int shown = settings.value(key, 0).toInt();
        if (shown >= 3) return;
        settings.setValue(key, shown + 1);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            qWarning() << "Could not save APK signature reminder count:" << settings.status();
        emit signatureReminder();
    }

    void readStandaloneRpmDiagnostics(bool final)
    {
        m_standaloneErrorBuffer += m_standaloneRpm.readAllStandardError();
        for (;;) {
            const int newline = m_standaloneErrorBuffer.indexOf('\n');
            if (newline < 0 && !final) break;
            if (newline < 0 && m_standaloneErrorBuffer.isEmpty()) break;
            const QByteArray line = newline < 0 ? m_standaloneErrorBuffer
                                                : m_standaloneErrorBuffer.left(newline);
            m_standaloneErrorBuffer = newline < 0 ? QByteArray()
                                                 : m_standaloneErrorBuffer.mid(newline + 1);
            QString stage;
            if (line == "ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:inspect") stage = tr("Inspecting the app and preparing the package...");
            else if (line == "ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:build") stage = tr("Building the RPM with the app and runtime...");
            else if (line == "ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:sign") stage = tr("Signing the RPM...");
            else if (line == "ANDROID_COMPATIBILITY_LAYER_RPM_STAGE:save") stage = tr("Saving the RPM in the library...");
            if (!stage.isEmpty()) {
                m_standaloneRpmStatus = stage;
                emit standaloneRpmChanged();
            } else {
                m_standaloneDiagnostic += line + '\n';
                if (m_standaloneDiagnostic.size() > 65536)
                    m_standaloneDiagnostic = m_standaloneDiagnostic.right(65536);
            }
        }
        if (m_standaloneErrorBuffer.size() > 65536) {
            m_standaloneDiagnostic = (m_standaloneDiagnostic + m_standaloneErrorBuffer).right(65536);
            m_standaloneErrorBuffer.clear();
        }
    }

    void finishOfficialStore(bool installed, const QString &message)
    {
        const bool open = installed && m_officialStoreOpenAfter;
        const bool online = m_officialStoreOnline;
        m_officialStoreDownload.clear();
        m_officialStoreActive = false;
        m_officialStoreOnline = false;
        m_officialStoreOpenAfter = false;
        m_officialStoreStatus = open
            ? tr("%1 Install Aurora Store as a standalone RPM from the app library.").arg(message)
            : message;
        m_result = message;
        if (online) emit officialStoreDownloadNotification(m_officialStoreStatus, -1, true);
        emit resultChanged();
        emit officialStoreChanged();
    }
    void startOfficialStore(const QString &path, bool openAfter)
    {
        if (officialStoreBusy() || m_externalInstall.busy()) {
            m_officialStoreStatus = tr("Another installation is in progress.");
            emit officialStoreChanged();
            return;
        }
        m_officialStoreActive = true;
        m_officialStoreOnline = path.isEmpty();
        m_officialStoreOpenAfter = openAfter;
        QString error;
        if (m_officialStoreOnline) {
            const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (!m_officialStoreDownload.start(cache, &error)) {
                finishOfficialStore(false, error);
                return;
            }
            m_officialStoreProgress = -1;
            m_officialStoreStatus = tr("Checking the latest Aurora Store release on GitLab...");
            emit officialStoreDownloadNotification(m_officialStoreStatus, -1, false);
        } else {
            if (!m_externalInstall.start(path, &error, QStringLiteral("com.aurora.store"), true)) {
                finishOfficialStore(false, error);
                return;
            }
            m_officialStoreStatus = tr("Inspecting the selected APK...");
            emit externalInstallBusyChanged();
        }
        emit officialStoreChanged();
    }
    void startMicrogUpdate(const QString &path, bool openAfter)
    {
        QString error;
        if (!m_microgInstall.start(path, [this]() { return !previewRunning(); }, &error)) {
            m_microgStatus = error;
        } else {
            m_microgOnline = path.isEmpty();
            m_microgProgress = -1;
            m_microgOpenAfter = openAfter;
            m_microgStatus = path.isEmpty() ? tr("Checking the latest microG release...")
                                             : tr("Inspecting the selected microG APK...");
            if (m_microgOnline) emit microgDownloadNotification(m_microgStatus, -1, false);
        }
        emit microgChanged();
    }
    void launchMicrog(const QString &path)
    {
        mg_apk_info info = {};
        const QByteArray apk = path.toUtf8();
        if (!mg_apk_read_info(apk.constData(), &info) || !info.runnable) {
            m_microgStatus = tr("microG is enabled, but this APK has no supported launchable settings screen.");
            return;
        }
        const QString key = QStringLiteral("__microg__");
        if (m_sessions.running(key)) {
            QString error;
            if (!m_sessions.activate(key, 3000, &error))
                m_microgStatus = tr("Could not switch to microG: %1").arg(error);
            return;
        }
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (base.isEmpty() || !startMicrogPreview(QDir(base).filePath(QStringLiteral("services/data")), path)) {
            m_microgStatus = tr("Could not start microG. See the app log.");
            return;
        }
        m_microgStatus = tr("Starting microG; compatibility is not guaranteed.");
    }
    static QString microgApkPath()
    {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("services/GmsCore.apk"));
    }
    void saveGoogleSetting(const QString &key, const QString &value)
    {
        if (RuntimeSettings::save(key, value)) m_runtimeSettingsError.clear();
        else m_runtimeSettingsError = tr("Could not save Google services settings.");
        emit runtimeSettingsChanged();
    }
    QString m_runtimeSettingsError;
    void clearConfirmation()
    {
        m_externalPrompt = false;
        m_bundlePrompt = false;
        m_installVisible = false;
        m_installToken.clear();
        emit installConfirmationChanged();
        QTimer::singleShot(0, this, [this]() { showNextConfirmation(); });
    }

    void unavailableConfirmation()
    {
        const QString token = m_installToken;
        const bool external = m_externalPrompt;
        const bool bundle = m_bundlePrompt;
        clearConfirmation();
        if (bundle) m_bundleInstall.unavailable();
        else if (external) m_externalInstall.unavailable();
        else if (!token.isEmpty()) m_sessions.unavailable(token);
    }

    void enqueueConfirmation(const QString &token, const QString &label, const QString &package,
                             quint32 version, bool external, bool bundle = false)
    {
        m_pendingInstalls.enqueue(PendingInstall{token, label, package, version, external, bundle});
        showNextConfirmation();
    }

    void showNextConfirmation()
    {
        if (!m_installToken.isEmpty() || m_pendingInstalls.isEmpty()) return;
        const PendingInstall pending = m_pendingInstalls.dequeue();
        m_installToken = pending.token;
        m_installLabel = pending.label;
        m_installPackage = pending.package;
        m_installVersion = pending.version;
        m_externalPrompt = pending.external;
        m_bundlePrompt = pending.bundle;
        const QString token = m_installToken;
        if (!m_confirmationWindow || !m_installUiReady || m_confirmationWindow->status() != QQuickView::Ready) {
            unavailableConfirmation();
            return;
        }
        m_confirmationWindow->show();
        m_confirmationWindow->raise();
        m_confirmationWindow->requestActivate();
        QTimer::singleShot(1500, this, [this, token]() {
            if (m_installToken != token) return;
            if (!m_confirmationWindow || !m_installUiReady || m_confirmationWindow->status() != QQuickView::Ready ||
                !m_confirmationWindow->isActive() || !m_confirmationWindow->isVisible()) {
                unavailableConfirmation();
                return;
            }
            m_installVisible = true;
            emit installConfirmationChanged();
        });
    }

    bool startMicrogPreview(const QString &data, const QString &apk)
    {
        if (microgBusy()) {
            m_appResult = tr("Wait for the microG update to finish before starting an app.");
            return false;
        }
        const QString key = QStringLiteral("__microg__");
        QString error;
        if (m_sessions.start(key, m_androidRoot, data, apk, appStoragePath(), &error)) return true;
        m_appResult = tr("Could not start %1: %2").arg(key, error);
        qWarning().noquote() << m_appResult;
        return false;
    }

    static int bundleDensityDpi()
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen) return 160;
        return previewViewport(screen->size().width(), screen->size().height(),
                               screen->devicePixelRatio(), RuntimeSettings::renderCap()).density;
    }

    static QString appStoragePath()
    {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("apps"));
    }
    QString m_androidRoot;
    QProcess m_standaloneRpm;
    QString m_standalonePackage;
    QByteArray m_standaloneErrorBuffer;
    QByteArray m_standaloneDiagnostic;
    QTimer m_standaloneTimeout;
    bool m_standaloneTimedOut = false;
    bool m_standaloneRpmOpened = false;
    QString m_standaloneRpmStatus;
    ApkStore m_store;
    mutable QVariantList m_cachedInstalledApps;
    mutable bool m_installedAppsValid = false;
    ExternalApkInstall m_externalInstall;
    ApkBundleInstall m_bundleInstall;
    MicrogInstall m_microgInstall;
    OfficialStoreDownload m_officialStoreDownload;
    DiagnosticLog *m_diagnosticLog;
    PreviewSessions m_sessions;
    LogChannel m_logChannel;
    QPointer<QQuickView> m_confirmationWindow;
    QQueue<PendingInstall> m_pendingInstalls;
    QSet<QString> m_forcedClosing;
    QStringList m_logOrder;
    QMap<QString, QString> m_logKeys, m_logTexts;
    quint32 m_installVersion = 0;
    bool m_installVisible = false;
    bool m_externalPrompt = false;
    bool m_bundlePrompt = false;
    bool m_installUiReady = false;
    bool m_logFailed = false;
    QString m_installToken, m_installLabel, m_installPackage;
    QString m_result;
    QString m_appResult;
    QString m_microgStatus;
    bool m_microgOnline = false;
    int m_microgProgress = -1;
    bool m_microgOpenAfter = false;
    QString m_officialStoreStatus;
    int m_officialStoreProgress = -1;
    bool m_officialStoreActive = false;
    bool m_officialStoreOnline = false;
    bool m_officialStoreOpenAfter = false;
};

#endif
