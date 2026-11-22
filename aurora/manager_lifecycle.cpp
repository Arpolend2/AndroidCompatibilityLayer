#include "manager_lifecycle.h"
#include "standalone_diagnostic_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
const char ServiceName[] = "ru.androidcompatibilitylayer.appmanager";
const char ObjectPath[] = "/ru/androidcompatibilitylayer/appmanager";
const qint64 maxAppMetadataBytes = 1024 * 1024;
const qint64 maxAppsListBytes = 4 * 1024 * 1024;

bool readBoundedFile(const QString &path, qint64 limit, QByteArray *contents, bool *exists,
                     QString *error)
{
    QFile file(path);
    *exists = file.exists();
    if (!*exists) {
        contents->clear();
        return true;
    }
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) ||
        file.size() < 0 || file.size() > limit) {
        if (error) *error = QStringLiteral("Cannot safely read shared microG metadata: %1").arg(path);
        return false;
    }
    *contents = file.readAll();
    if (file.error() != QFileDevice::NoError || contents->size() > limit) {
        if (error) *error = QStringLiteral("Cannot read bounded shared microG metadata: %1").arg(path);
        return false;
    }
    return true;
}

bool writeAtomicFile(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        if (error) *error = QStringLiteral("Cannot atomically write shared microG metadata: %1").arg(path);
        return false;
    }
    return true;
}
}

ManagerLifecycle::ManagerLifecycle(const std::function<bool()> &showGui, QObject *parent,
                                   int idleMilliseconds)
    : QObject(parent), m_showGui(showGui), m_provider(QDBusConnection::sessionBus(), nullptr,
                                                    [this]() { noteRequest(); })
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(idleMilliseconds);
    connect(&m_idle, &QTimer::timeout, QCoreApplication::instance(), &QCoreApplication::quit);
    m_receiver.setActivationHandler([this]() { return this->showGui(); });
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
    connect(&m_receiver, &ExternalApkReceiver::diagnosticRequested, this, [this]() { noteRequest(); });
#endif
    connect(&m_receiver, &ExternalApkReceiver::requested, this, [this](const QString &path) {
        if (this->showGui()) emit fileRequested(path);
        else qWarning("Cannot display an incoming file request.");
    });
    connect(&m_receiver, &ExternalApkReceiver::rejected, this, [this](const QString &message) {
        emit fileRejected(message);
    });
    connect(&m_worker, &SharedMicrogSupervisor::workerDied,
            &m_provider, &SharedMicrogProviderObject::revokeAllChannels);
    m_microgRestart.setSingleShot(true);
    connect(&m_worker, &SharedMicrogSupervisor::workerDied, this, [this]() {
        if (m_microgBackground) m_microgRestart.start(30000);
    });
    connect(&m_microgRestart, &QTimer::timeout, this, [this]() {
        if (!m_microgBackground) return;
        QString error;
        if (!m_worker.startRetained(&error)) {
            qWarning("Cannot start the background microG worker: %s", qPrintable(error));
            if (m_microgBackground) m_microgRestart.start(30000);
        }
    });
    m_provider.setWorkerCheckHandler([this](QString *error) {
        if (!m_microgBackground) {
            if (error) *error = QStringLiteral("The shared microG worker is not configured.");
            return false;
        }
        if (m_microgRestart.isActive() && m_microgRestart.remainingTime() > 0) {
            if (error) *error = QStringLiteral("The shared microG worker is waiting to retry.");
            return false;
        }
        if (!m_worker.startRetained(error)) {
            if (m_microgBackground) m_microgRestart.start(30000);
            return false;
        }
        m_microgRestart.stop();
        return true;
    });
    connect(&m_worker, &SharedMicrogSupervisor::channelDisconnected,
            &m_provider, &SharedMicrogProviderObject::releaseChannelById);
}

ManagerLifecycle::~ManagerLifecycle()
{
    m_provider.revokeAllChannels();
    m_worker.stop();
}

bool ManagerLifecycle::configureSharedMicrogWorker(const QString &program,
                                                   const QStringList &arguments,
                                                   const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                                   QString *error)
{
    return configureSharedMicrogWorker(program, arguments, packageResolver,
                                       SharedMicrogProviderObject::ApkMetadataResolver(), error);
}

bool ManagerLifecycle::configureSharedMicrogWorker(const QString &program,
                                                   const QStringList &arguments,
                                                   const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                                   const SharedMicrogProviderObject::ApkMetadataResolver &metadataResolver,
                                                   QString *error)
{
    if (!packageResolver) {
        if (error) *error = QStringLiteral("A trusted shared microG package resolver is required.");
        return false;
    }
    if (!m_worker.configure(program, arguments, error)) return false;
    if (!m_provider.setPackageResolver(packageResolver, error)) return false;
    if (metadataResolver && !m_provider.setApkMetadataResolver(metadataResolver, error)) return false;
    m_appsDirectory.clear();
    for (const QString &argument : arguments) {
        if (argument.startsWith(QStringLiteral("--apps-dir="))) {
            m_appsDirectory = argument.mid(QStringLiteral("--apps-dir=").size());
            break;
        }
    }
    const SharedMicrogProviderObject::ChannelHandler open =
        [this](const QString &, const QString &package, const QByteArray &appMetadata,
               int uid, int pid, quint64 channelId,
               const QDBusUnixFileDescriptor &server, QString *workerError) {
            if (!server.isValid()) {
                if (workerError) *workerError = QStringLiteral("Invalid shared microG stream.");
                return false;
            }
            if (!appMetadata.isEmpty() &&
                !publishClientAppMetadata(package, appMetadata, channelId, workerError))
                return false;
            if (m_worker.openChannel(package, uid, pid, channelId,
                                     server.fileDescriptor(), workerError))
                return true;
            releaseClientAppMetadata(channelId);
            return false;
        };
    const SharedMicrogProviderObject::ChannelReleaseHandler close =
        [this](const QString &, quint64 channelId) {
            m_worker.closeChannel(channelId);
            releaseClientAppMetadata(channelId);
        };
    return m_provider.setChannelHandlers(open, close, error);
}

bool ManagerLifecycle::publishClientAppMetadata(const QString &package, const QByteArray &payload,
                                               quint64 channelId, QString *error)
{
    if (error) error->clear();
    static const QRegularExpression packagePattern(QStringLiteral(
        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    if (m_appsDirectory.isEmpty() || !packagePattern.match(package).hasMatch() ||
        payload.isEmpty() || payload.size() > maxAppMetadataBytes || m_clientAppChannels.contains(channelId)) {
        if (error) *error = QStringLiteral("Invalid or oversized shared microG client metadata.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("The shared microG client metadata is malformed.");
        return false;
    }
    const QJsonObject metadata = document.object();
    auto safeField = [](QString value) {
        value.replace(QLatin1Char('\t'), QLatin1Char(' '));
        value.replace(QLatin1Char('\r'), QLatin1Char(' '));
        value.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return value;
    };
    const QString activity = safeField(metadata.value(QStringLiteral("activity")).toString());
    const QString label = safeField(metadata.value(QStringLiteral("label")).toString());
    const QString version = safeField(metadata.value(QStringLiteral("versionName")).toString());
    const QString versionCode = metadata.value(QStringLiteral("versionCode")).toString();
    const QJsonValue activitiesValue = metadata.value(QStringLiteral("activities"));
    if (metadata.value(QStringLiteral("package")).toString() != package ||
        activity.size() > 200 || label.size() > 1024 || version.size() > 256 ||
        versionCode.size() > 32 || !QRegularExpression(QStringLiteral("\\A[0-9]+\\z"))
            .match(versionCode).hasMatch() || !activitiesValue.isObject()) {
        if (error) *error = QStringLiteral("The shared microG client metadata does not match the caller.");
        return false;
    }
    const QByteArray activities = QJsonDocument(activitiesValue.toObject()).toJson(QJsonDocument::Compact);
    if (activities.isEmpty() || activities.size() > maxAppMetadataBytes) {
        if (error) *error = QStringLiteral("The shared microG APK activities exceed the metadata limit.");
        return false;
    }
    const QJsonObject activityInfo = activitiesValue.toObject();
    const QJsonArray certificates = activityInfo.value(QStringLiteral("certificates")).toArray();
    if (activityInfo.value(QStringLiteral("package")).toString() != package ||
        certificates.isEmpty() || certificates.size() > 8) {
        if (error) *error = QStringLiteral("The shared microG APK description is invalid.");
        return false;
    }
    for (const QJsonValue &certificate : certificates) {
        const QString hex = certificate.toString();
        if (certificate.type() != QJsonValue::String || hex.isEmpty() || hex.size() > 32768 ||
            (hex.size() & 1) || !QRegularExpression(QStringLiteral("\\A[0-9A-Fa-f]+\\z"))
                .match(hex).hasMatch()) {
            if (error) *error = QStringLiteral("The shared microG APK certificate data is invalid.");
            return false;
        }
    }

    const QByteArray appsLine = (package + QLatin1Char('\t') + activity + QLatin1Char('\t') +
                                 label + QLatin1Char('\t') + version + QLatin1Char('\t') +
                                 versionCode + QLatin1Char('\n')).toUtf8();
    auto existing = m_clientAppMetadata.find(package);
    if (existing != m_clientAppMetadata.end()) {
        if (existing->payload != payload) {
            if (error) *error = QStringLiteral("A different APK version of this package is already connected to shared microG.");
            return false;
        }
        existing->channels.insert(channelId);
        m_clientAppChannels.insert(channelId, package);
        return true;
    }

    if (QFileInfo(m_appsDirectory).isSymLink() || !QDir().mkpath(m_appsDirectory)) {
        if (error) *error = QStringLiteral("The shared microG apps directory is unavailable.");
        return false;
    }
    ClientAppMetadata state;
    state.payload = payload;
    state.appsLine = appsLine;
    const QString appsPath = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray appsText;
    if (!readBoundedFile(appsPath, maxAppsListBytes, &appsText, &state.hadAppsFile, error))
        return false;
    const QList<QByteArray> lines = appsText.split('\n');
    for (const QByteArray &line : lines) {
        if (line.left(line.indexOf('\t') < 0 ? line.size() : line.indexOf('\t')) == package.toUtf8())
            state.previousAppsLines.append(line + (line.isEmpty() ? QByteArray() : QByteArray("\n")));
    }
    QByteArray previousActivities;
    const QString packageDirectory = QDir(m_appsDirectory).filePath(package);
    const QString activitiesPath = QDir(packageDirectory).filePath(QStringLiteral("activities.json"));
    if (QFileInfo(packageDirectory).isSymLink() ||
        (QFileInfo(packageDirectory).exists() && !QFileInfo(packageDirectory).isDir()) ||
        !readBoundedFile(activitiesPath, maxAppMetadataBytes, &previousActivities,
                         &state.hadActivities, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The shared microG package metadata path is unsafe.");
        return false;
    }
    state.previousActivities = previousActivities;
    if (!QDir().mkpath(packageDirectory) ||
        !writeAtomicFile(activitiesPath, activities, error)) return false;

    state.channels.insert(channelId);
    m_clientAppMetadata.insert(package, state);
    m_clientAppChannels.insert(channelId, package);
    if (!writeClientAppsList(error)) {
        m_clientAppChannels.remove(channelId);
        m_clientAppMetadata.remove(package);
        if (state.hadActivities)
            writeAtomicFile(activitiesPath, state.previousActivities, nullptr);
        else if (QFileInfo::exists(activitiesPath) && !QFile::remove(activitiesPath))
            qWarning("Cannot remove rejected shared microG APK metadata: %s", qPrintable(activitiesPath));
        return false;
    }
    return true;
}

bool ManagerLifecycle::writeClientAppsList(QString *error)
{
    if (m_appsDirectory.isEmpty() || QFileInfo(m_appsDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("The shared microG apps directory is unavailable.");
        return false;
    }
    const QString path = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray current;
    bool exists = false;
    if (!readBoundedFile(path, maxAppsListBytes, &current, &exists, error)) return false;
    QByteArray updated;
    const QList<QByteArray> lines = current.split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) continue;
        const int tab = line.indexOf('\t');
        const QByteArray package = line.left(tab < 0 ? line.size() : tab);
        if (!m_clientAppMetadata.contains(QString::fromUtf8(package)))
            updated += line + '\n';
    }
    for (auto it = m_clientAppMetadata.constBegin(); it != m_clientAppMetadata.constEnd(); ++it)
        updated += it->appsLine;
    if (updated.size() > maxAppsListBytes) {
        if (error) *error = QStringLiteral("The shared microG apps list exceeds the metadata limit.");
        return false;
    }
    if (updated.isEmpty() && !exists) return true;
    return writeAtomicFile(path, updated, error);
}

void ManagerLifecycle::releaseClientAppMetadata(quint64 channelId)
{
    const auto channel = m_clientAppChannels.find(channelId);
    if (channel == m_clientAppChannels.end()) return;
    const QString package = channel.value();
    m_clientAppChannels.erase(channel);
    auto state = m_clientAppMetadata.find(package);
    if (state == m_clientAppMetadata.end()) return;
    state->channels.remove(channelId);
    if (!state->channels.isEmpty()) return;

    const ClientAppMetadata previous = state.value();
    const QString appsPath = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray appsText;
    bool exists = false;
    QString error;
    if (readBoundedFile(appsPath, maxAppsListBytes, &appsText, &exists, &error)) {
        const QList<QByteArray> lines = appsText.split('\n');
        QByteArray restored;
        bool replacedInjectedLine = false;
        for (const QByteArray &line : lines) {
            if (line.isEmpty()) continue;
            if (line + '\n' == previous.appsLine) {
                if (!replacedInjectedLine) {
                    for (const QByteArray &oldLine : previous.previousAppsLines)
                        restored += oldLine;
                    replacedInjectedLine = true;
                }
            } else {
                restored += line + '\n';
            }
        }
        if (replacedInjectedLine && restored.isEmpty() && !previous.hadAppsFile) {
            if (QFileInfo::exists(appsPath) && !QFile::remove(appsPath))
                qWarning("Cannot remove temporary shared microG apps list: %s", qPrintable(appsPath));
        } else if ((!restored.isEmpty() || exists) && !writeAtomicFile(appsPath, restored, &error)) {
            qWarning("Cannot restore shared microG apps list: %s", qPrintable(error));
        }
    } else {
        qWarning("Cannot restore shared microG apps list: %s", qPrintable(error));
    }

    const QString activitiesPath = QDir(m_appsDirectory).filePath(
        package + QStringLiteral("/activities.json"));
    QByteArray activities;
    bool activitiesExist = false;
    if (!QFileInfo(QDir(m_appsDirectory).filePath(package)).isSymLink() &&
        readBoundedFile(activitiesPath, maxAppMetadataBytes, &activities,
                        &activitiesExist, &error) &&
        activitiesExist) {
        const QJsonObject metadata = QJsonDocument::fromJson(previous.payload).object();
        const QByteArray injected = QJsonDocument(metadata.value(QStringLiteral("activities")).toObject())
                                       .toJson(QJsonDocument::Compact);
        if (activities == injected) {
            if (previous.hadActivities) {
                if (!writeAtomicFile(activitiesPath, previous.previousActivities, &error))
                    qWarning("Cannot restore shared microG APK description: %s", qPrintable(error));
            } else if (!QFile::remove(activitiesPath)) {
                qWarning("Cannot remove temporary shared microG APK description: %s", qPrintable(activitiesPath));
            }
        }
    } else if (!error.isEmpty()) {
        qWarning("Cannot restore shared microG APK description: %s", qPrintable(error));
    }
    m_clientAppMetadata.erase(state);
}

void ManagerLifecycle::disableSharedMicrogWorker()
{
    m_microgBackground = false;
    m_microgRestart.stop();
    m_worker.releaseRetention();
    m_provider.revokeAllChannels();
    m_worker.stop();
    QString error;
    if (!m_provider.clearBackend(&error))
        qWarning("Cannot disable shared microG backend: %s", qPrintable(error));
}

void ManagerLifecycle::enableSharedMicrogBackground()
{
    m_microgBackground = true;
    m_microgRestart.start(0);
}

SharedMicrogProviderObject::PackageResolver ManagerLifecycle::registryPackageResolver(
    const QString &identityDirectory)
{
    return [identityDirectory](const QString &executable, const QString &identityKey, QString *error) {
        if (error) error->clear();
        if (identityKey.isEmpty()) {
            if (error) *error = QStringLiteral("Caller identity is incomplete.");
            return QString();
        }
        const QString appId = StandaloneDiagnosticRegistry::appIdFromExecutable(executable);
        if (appId.isEmpty()) {
            if (error) *error = QStringLiteral("Caller is not a registered standalone app.");
            return QString();
        }
        const QString package = StandaloneDiagnosticRegistry::lookup(identityDirectory, appId);
        if (package.isEmpty() && error)
            *error = QStringLiteral("No exported package mapping exists for this caller.");
        return package;
    };
}

bool ManagerLifecycle::showGui()
{
    if (!m_showGui || !m_showGui()) return false;
    m_background = false;
    m_idle.stop();
    return true;
}

void ManagerLifecycle::noteRequest()
{
    if (m_background && !m_keepAlive) m_idle.start();
}

void ManagerLifecycle::setKeepAlive(bool keepAlive)
{
    m_keepAlive = keepAlive;
    if (keepAlive) m_idle.stop();
    else if (m_background) m_idle.start();
}

bool ManagerLifecycle::start(bool background, QString *error)
{
    if (error) error->clear();
    if (!m_receiver.start(error)) return false;
    if (!m_receiver.ownsLegacyService()) {
        if (background) {
            if (error) *error = QStringLiteral("The manager D-Bus service is unavailable.");
            return false;
        }
        qWarning("Shared microG provider service is unavailable: manager D-Bus name not owned.");
        return true;
    }
    QString providerError;
    if (!m_provider.start(&providerError)) {
        if (background) {
            if (error) *error = providerError;
            return false;
        }
        qWarning("Shared microG provider service is unavailable: %s", qPrintable(providerError));
    }
    m_background = background;
    if (background && !m_keepAlive) m_idle.start();
    return true;
}

QStringList ManagerLifecycle::launchFileArguments(const QStringList &arguments)
{
    // Aurora debug launches can leave the desktop Exec field code unexpanded.
    if (arguments.size() == 1 && arguments.first() == QLatin1String("%u"))
        return QStringList();
    return arguments;
}

ManagerLifecycle::ForwardResult ManagerLifecycle::forwardToRunningManager(
    const QStringList &arguments, QString *error)
{
    if (error) error->clear();
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        if (error) *error = QStringLiteral("Session D-Bus is unavailable.");
        return Unavailable;
    }
    const QDBusReply<bool> registered = bus.interface()->isServiceRegistered(QString::fromLatin1(ServiceName));
    if (!registered.isValid()) {
        if (error) *error = registered.error().message();
        return Unavailable;
    }
    if (!registered.value()) return NoOwner;
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(ServiceName), QString::fromLatin1(ObjectPath),
        QString::fromLatin1(ServiceName), QStringLiteral("activate"));
    message.setAutoStartService(false);
    message << arguments;
    const QDBusReply<bool> result = bus.call(message, QDBus::Block, 5000);
    if (!result.isValid()) {
        if (error) *error = result.error().name() + QStringLiteral(": ") + result.error().message();
        return Failed;
    }
    if (!result.value()) {
        if (error) *error = QStringLiteral("The running manager could not open its window.");
        return Failed;
    }
    return Delivered;
}
