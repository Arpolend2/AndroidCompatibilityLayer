#include "manager_lifecycle.h"
#include "standalone_diagnostic_registry.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>

namespace {
const char ServiceName[] = "ru.androidcompatibilitylayer.appmanager";
const char ObjectPath[] = "/ru/androidcompatibilitylayer/appmanager";
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
    if (!packageResolver) {
        if (error) *error = QStringLiteral("A trusted shared microG package resolver is required.");
        return false;
    }
    if (!m_worker.configure(program, arguments, error)) return false;
    if (!m_provider.setPackageResolver(packageResolver, error)) return false;
    const SharedMicrogProviderObject::ChannelHandler open =
        [this](const QString &, const QString &package, int uid, int pid, quint64 channelId,
               const QDBusUnixFileDescriptor &server, QString *workerError) {
            if (!server.isValid()) {
                if (workerError) *workerError = QStringLiteral("Invalid shared microG stream.");
                return false;
            }
            return m_worker.openChannel(package, uid, pid, channelId,
                                        server.fileDescriptor(), workerError);
        };
    const SharedMicrogProviderObject::ChannelReleaseHandler close =
        [this](const QString &, quint64 channelId) { m_worker.closeChannel(channelId); };
    return m_provider.setChannelHandlers(open, close, error);
}

void ManagerLifecycle::disableSharedMicrogWorker()
{
    m_provider.revokeAllChannels();
    m_worker.stop();
    QString error;
    if (!m_provider.clearBackend(&error))
        qWarning("Cannot disable shared microG backend: %s", qPrintable(error));
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
