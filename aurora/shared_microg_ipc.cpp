#include "shared_microg_ipc.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const char serviceName[] = "ru.androidcompatibilitylayer.appmanager";
const char objectPath[] = "/ru/androidcompatibilitylayer/appmanager/microg";
const char interfaceName[] = "ru.androidcompatibilitylayer.appmanager.microg";
const int maxPendingIdentifications = 16;

bool validProvider(const QString &provider)
{
    return provider == QLatin1String("none") || provider == QLatin1String("microg");
}

bool readProviderSetting(QString *provider, QString *error)
{
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        *error = QStringLiteral("Cannot read the manager settings.");
        return false;
    }
    *provider = settings.value(QStringLiteral("googleServices/provider"), QStringLiteral("none")).toString();
    if (settings.status() != QSettings::NoError || !validProvider(*provider)) {
        *error = QStringLiteral("Invalid Google services provider setting.");
        return false;
    }
    return true;
}

void sendIdentificationReply(const QDBusConnection &connection, const QDBusMessage &reply)
{
    if (!connection.send(reply))
        qWarning("Shared microG: could not send identification reply: %s",
                 qPrintable(connection.lastError().message()));
}
}

SharedMicrogProviderObject::SharedMicrogProviderObject(const QDBusConnection &bus, QObject *parent,
                                                       const std::function<void()> &onRequest,
                                                       const QString &identityDirectory,
                                                       const ChannelHandler &channelHandler,
                                                       const PackageResolver &packageResolver)
    : QDBusVirtualObject(parent), m_bus(bus),
      m_senderWatcher(new QDBusServiceWatcher(this)),
      m_identityDirectory(identityDirectory.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : identityDirectory),
      m_onRequest(onRequest), m_channelHandler(channelHandler), m_packageResolver(packageResolver)
{
    m_providerCheck.setInterval(1000);
    connect(&m_providerCheck, &QTimer::timeout, this, &SharedMicrogProviderObject::refreshProviderSetting);
    m_senderWatcher->setConnection(bus);
    m_senderWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_senderWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &sender) {
        revokeChannel(sender);
        m_virtualPids.remove(sender);
        m_senderWatcher->removeWatchedService(sender);
    });
}

SharedMicrogProviderObject::~SharedMicrogProviderObject()
{
    revokeAllChannels();
    if (m_registered)
        m_bus.unregisterObject(QString::fromLatin1(objectPath));
}

bool SharedMicrogProviderObject::setChannelHandlers(const ChannelHandler &channelHandler,
                                                    const ChannelReleaseHandler &releaseHandler,
                                                    QString *error)
{
    if (error) error->clear();
    if (!m_channels.isEmpty() || !channelHandler || !releaseHandler) {
        if (error) *error = QStringLiteral("Cannot configure the shared microG backend.");
        return false;
    }
    m_channelHandler = channelHandler;
    m_channelReleaseHandler = releaseHandler;
    return true;
}

bool SharedMicrogProviderObject::setPackageResolver(const PackageResolver &packageResolver,
                                                    QString *error)
{
    if (error) error->clear();
    if (!m_channels.isEmpty() || !packageResolver) {
        if (error) *error = QStringLiteral("Cannot configure the shared microG identity resolver.");
        return false;
    }
    m_packageResolver = packageResolver;
    return true;
}

bool SharedMicrogProviderObject::clearBackend(QString *error)
{
    if (error) error->clear();
    if (!m_channels.isEmpty()) {
        if (error) *error = QStringLiteral("Cannot clear the shared microG backend while channels are active.");
        return false;
    }
    m_channelHandler = ChannelHandler();
    m_channelReleaseHandler = ChannelReleaseHandler();
    m_packageResolver = PackageResolver();
    return true;
}

bool SharedMicrogProviderObject::start(QString *error)
{
    if (error) error->clear();
    if (m_registered) {
        if (error) *error = QStringLiteral("The shared microG provider object is already registered.");
        return false;
    }
    if (!m_bus.isConnected() || !m_bus.interface()) {
        if (error) *error = QStringLiteral("Session D-Bus is unavailable.");
        return false;
    }
    const QDBusReply<QString> owner = m_bus.interface()->serviceOwner(QString::fromLatin1(serviceName));
    if (!owner.isValid() || owner.value() != m_bus.baseService()) {
        if (error) *error = QStringLiteral("This process does not own the manager D-Bus service.");
        return false;
    }
    if (!m_bus.registerVirtualObject(QString::fromLatin1(objectPath), this, QDBusConnection::SingleNode)) {
        if (error) *error = m_bus.lastError().message();
        return false;
    }
    m_registered = true;
    return true;
}

QString SharedMicrogProviderObject::introspect(const QString &path) const
{
    if (path != QLatin1String(objectPath))
        return QString();
    QString result = QStringLiteral("<interface name=\"ru.androidcompatibilitylayer.appmanager.microg\">"
                          "<method name=\"GetProvider\"><arg type=\"s\" direction=\"out\"/>"
                          "</method>"
                          "<method name=\"InspectCaller\"><arg type=\"as\" direction=\"out\"/>"
                          "</method>"
                          "<method name=\"ResolveCaller\"><arg type=\"as\" direction=\"out\"/>"
                          "</method>");
    if (m_channelHandler)
        result += QStringLiteral("<method name=\"OpenChannel\"><arg type=\"h\" direction=\"out\"/></method>"
                                 "<method name=\"OpenClientChannel\"><arg type=\"h\" direction=\"out\"/>"
                                 "<arg type=\"i\" direction=\"out\"/><arg type=\"i\" direction=\"out\"/>"
                                 "<arg type=\"s\" direction=\"out\"/></method>");
    return result + QStringLiteral("</interface>");
}

void SharedMicrogProviderObject::revokeChannel(const QString &sender)
{
    auto it = m_channels.find(sender);
    if (it == m_channels.end()) return;
    const quint64 channelId = it->id;
    const bool workerAccepted = it->workerAccepted;
    const int fd = it->server.fileDescriptor();
    if (::shutdown(fd, SHUT_RDWR) != 0 && errno != ENOTCONN)
        qWarning("Shared microG: could not revoke channel: %s", std::strerror(errno));
    m_channels.erase(it);
    if (m_channels.isEmpty()) m_providerCheck.stop();
    if (workerAccepted && m_channelReleaseHandler)
        m_channelReleaseHandler(sender, channelId);
}

void SharedMicrogProviderObject::refreshProviderSetting()
{
    if (m_channels.isEmpty()) return;
    QString provider, error;
    const bool valid = readProviderSetting(&provider, &error);
    if (valid && provider == QLatin1String("microg")) return;
    if (!valid)
        qWarning("Shared microG: revoking channels: %s", qPrintable(error));
    while (!m_channels.isEmpty()) revokeChannel(m_channels.constBegin().key());
}

void SharedMicrogProviderObject::releaseChannel(const QString &sender, quint64 channelId)
{
    const auto it = m_channels.constFind(sender);
    if (it != m_channels.constEnd() && it->id == channelId) revokeChannel(sender);
}

void SharedMicrogProviderObject::releaseChannelById(quint64 channelId)
{
    for (auto it = m_channels.constBegin(); it != m_channels.constEnd(); ++it) {
        if (it->id == channelId) {
            revokeChannel(it.key());
            return;
        }
    }
}

void SharedMicrogProviderObject::revokeAllChannels()
{
    while (!m_channels.isEmpty()) revokeChannel(m_channels.constBegin().key());
}

void SharedMicrogProviderObject::openChannel(const QDBusMessage &message, const QDBusConnection &connection,
                                            const QString &sender, const QString &package, int uid, int pid,
                                            bool withIdentity)
{
    QString provider, error;
    if (!readProviderSetting(&provider, &error)) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed, error));
        return;
    }
    if (provider != QLatin1String("microg")) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::AccessDenied,
            QStringLiteral("The shared microG provider is not selected.")));
        return;
    }
    if (m_channels.contains(sender) || m_nextChannelId == 0) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::LimitsExceeded,
            QStringLiteral("A channel for this caller is already open or channel IDs are exhausted.")));
        return;
    }
    QDBusUnixFileDescriptor server, client;
    if (!createSharedMicrogStreamPair(&server, &client, &error)) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed, error));
        return;
    }
    const quint64 channelId = m_nextChannelId++;
    m_channels.insert(sender, Channel{channelId, server, false});
    if (!m_providerCheck.isActive()) m_providerCheck.start();
    if (!m_channelHandler(sender, package, uid, pid, channelId, server, &error)) {
        releaseChannel(sender, channelId);
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed,
            error.isEmpty() ? QStringLiteral("The microG server refused the channel.") : error));
        return;
    }
    const auto it = m_channels.constFind(sender);
    if (it == m_channels.constEnd() || it->id != channelId) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed,
            QStringLiteral("The microG server closed the channel.")));
        return;
    }
    m_channels[sender].workerAccepted = true;
    QVariantList values;
    values << QVariant::fromValue(client);
    // Exactly the identity that the backend accepted for this channel.
    if (withIdentity) values << uid << pid << package;
    if (!connection.send(message.createReply(values))) {
        releaseChannel(sender, channelId);
        qWarning("Shared microG: could not send channel: %s", qPrintable(connection.lastError().message()));
    }
}

bool SharedMicrogProviderObject::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    if (message.type() != QDBusMessage::MethodCallMessage ||
        message.path() != QLatin1String(objectPath) ||
        message.interface() != QLatin1String(interfaceName) ||
        (message.member() != QLatin1String("GetProvider") &&
         message.member() != QLatin1String("InspectCaller") &&
         message.member() != QLatin1String("ResolveCaller") &&
         message.member() != QLatin1String("OpenChannel") &&
         message.member() != QLatin1String("OpenClientChannel")))
        return false;
    if (!message.arguments().isEmpty()) {
        connection.send(message.createErrorReply(QDBusError::InvalidArgs,
                                                QStringLiteral("This method accepts no arguments.")));
        return true;
    }
    const bool withIdentity = message.member() == QLatin1String("OpenClientChannel");
    const bool open = withIdentity || message.member() == QLatin1String("OpenChannel");
    if (open && !m_channelHandler) {
        sendIdentificationReply(connection, message.createErrorReply(QDBusError::NotSupported,
            QStringLiteral("The shared microG server is not available.")));
        return true;
    }
    if (open && !message.isReplyRequired()) {
        qWarning("Shared microG: channel requests require a reply.");
        return true;
    }
    if (message.member() == QLatin1String("InspectCaller") ||
        message.member() == QLatin1String("ResolveCaller") || open) {
        const bool resolve = message.member() != QLatin1String("InspectCaller");
        const QString sender = message.service();
        if (!sender.startsWith(QLatin1Char(':'))) {
            sendIdentificationReply(connection, message.createErrorReply(QDBusError::AccessDenied,
                                                     QStringLiteral("A D-Bus sender is required.")));
            return true;
        }
        if (m_pendingIdentifications >= maxPendingIdentifications) {
            sendIdentificationReply(connection, message.createErrorReply(QDBusError::LimitsExceeded,
                                                     QStringLiteral("Too many identification requests.")));
            return true;
        }
        if (m_onRequest) m_onRequest();
        QDBusMessage identify = QDBusMessage::createMethodCall(
            QStringLiteral("ru.omp.am"), QStringLiteral("/ru/omp/am"),
            QStringLiteral("ru.omp.am.Identify"), QStringLiteral("IdentifyByBusName"));
        identify << sender;
        ++m_pendingIdentifications;
        message.setDelayedReply(true);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            connection.asyncCall(identify, 5000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, connection, message, sender, resolve, open, withIdentity]() {
            --m_pendingIdentifications;
            const QDBusMessage reply = watcher->reply();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                sendIdentificationReply(connection, message.createErrorReply(reply.errorName(),
                    QStringLiteral("IdentifyByBusName failed: ") + reply.errorMessage()));
            } else {
                const QList<QVariant> values = reply.arguments();
                if (reply.type() != QDBusMessage::ReplyMessage || values.size() != 2 ||
                    values.at(0).type() != QVariant::String ||
                    values.at(1).type() != QVariant::String ||
                    values.at(0).toString().isEmpty() || values.at(0).toString().size() > 4096 ||
                    values.at(1).toString().size() > 256) {
                    sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed,
                        QStringLiteral("Invalid IdentifyByBusName response.")));
                } else if (!resolve) {
                    sendIdentificationReply(connection, message.createReply(QVariantList() <<
                        (QStringList() << sender << values.at(0).toString() << values.at(1).toString())));
                } else {
                    QString resolutionError;
                    const QString package = m_packageResolver
                        ? m_packageResolver(values.at(0).toString(), values.at(1).toString(),
                                            &resolutionError)
                        : QString();
                    static const QRegularExpression packagePattern(QStringLiteral(
                        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
                    if (values.at(1).toString().isEmpty() || package.size() > 255 ||
                        !packagePattern.match(package).hasMatch()) {
                        sendIdentificationReply(connection, message.createErrorReply(QDBusError::AccessDenied,
                            resolutionError.isEmpty() ? QStringLiteral("Unknown standalone caller.")
                                                      : resolutionError));
                    } else if (!m_virtualPids.contains(sender) &&
                               (m_virtualPids.size() >= 1024 || m_nextVirtualPid > 99999)) {
                        sendIdentificationReply(connection, message.createErrorReply(QDBusError::LimitsExceeded,
                            QStringLiteral("Virtual process identity limit reached.")));
                    } else {
                        const bool existing = m_virtualPids.contains(sender);
                        if (!existing) m_senderWatcher->addWatchedService(sender);
                        const QDBusReply<bool> alive = connection.interface()->isServiceRegistered(sender);
                        int uid = -1;
                        QString error;
                        if (!alive.isValid() || !alive.value()) {
                            if (!existing) m_senderWatcher->removeWatchedService(sender);
                            sendIdentificationReply(connection, message.createErrorReply(QDBusError::AccessDenied,
                                QStringLiteral("Caller disconnected.")));
                        } else if (!sharedMicrogUidForPackage(m_identityDirectory, package, &uid, &error)) {
                            if (!existing) m_senderWatcher->removeWatchedService(sender);
                            sendIdentificationReply(connection, message.createErrorReply(QDBusError::Failed, error));
                        } else {
                            if (!existing) {
                                m_virtualPids.insert(sender, m_nextVirtualPid++);
                            }
                            const int pid = m_virtualPids.value(sender);
                            if (open) openChannel(message, connection, sender, package, uid, pid,
                                                  withIdentity);
                            else sendIdentificationReply(connection, message.createReply(QVariantList() <<
                                (QStringList() << sender << package << QString::number(uid)
                                 << QString::number(pid))));
                        }
                    }
                }
            }
            watcher->deleteLater();
        });
        return true;
    }
    if (m_onRequest) m_onRequest();
    QString provider, error;
    if (!readProviderSetting(&provider, &error)) {
        connection.send(message.createErrorReply(QDBusError::Failed, error));
        return true;
    }
    connection.send(message.createReply(QVariantList() << provider));
    return true;
}

QDBusPendingReply<QString> requestSharedMicrogProvider(const QDBusConnection &bus, bool autoStart)
{
    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(serviceName), QString::fromLatin1(objectPath),
        QString::fromLatin1(interfaceName), QStringLiteral("GetProvider"));
    request.setAutoStartService(autoStart);
    return bus.asyncCall(request, 15000);
}

bool readSharedMicrogProvider(const QDBusPendingReply<QString> &reply, QString *provider, QString *error)
{
    if (provider) provider->clear();
    if (error) error->clear();
    if (!reply.isFinished()) {
        if (error) *error = QStringLiteral("The provider request has not finished.");
        return false;
    }
    if (reply.isError()) {
        if (error) *error = reply.error().name() + QStringLiteral(": ") + reply.error().message();
        return false;
    }
    if (!validProvider(reply.value())) {
        if (error) *error = QStringLiteral("The manager returned an invalid provider.");
        return false;
    }
    if (provider) *provider = reply.value();
    return true;
}

bool createSharedMicrogStreamPair(QDBusUnixFileDescriptor *local, QDBusUnixFileDescriptor *peer,
                                  QString *error)
{
    if (error) error->clear();
    if (!local || !peer || local == peer || local->isValid() || peer->isValid()) {
        if (error) *error = QStringLiteral("Two empty stream descriptors are required.");
        return false;
    }
    if (!QDBusUnixFileDescriptor::isSupported()) {
        if (error) *error = QStringLiteral("D-Bus file descriptor passing is unavailable.");
        return false;
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    local->giveFileDescriptor(sockets[0]);
    peer->giveFileDescriptor(sockets[1]);
    return true;
}

int takeSharedMicrogStream(QDBusUnixFileDescriptor *descriptor, QString *error)
{
    if (error) error->clear();
    if (!descriptor || !descriptor->isValid()) {
        if (error) *error = QStringLiteral("A stream descriptor is required.");
        return -1;
    }
    const int fd = descriptor->fileDescriptor();
    int type = 0;
    socklen_t typeSize = sizeof(type);
    sockaddr_un address;
    socklen_t addressSize = sizeof(address);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeSize) != 0 ||
        type != SOCK_STREAM || ::getpeername(fd, reinterpret_cast<sockaddr *>(&address), &addressSize) != 0 ||
        address.sun_family != AF_UNIX) {
        if (error) *error = QStringLiteral("The descriptor is not a connected Unix stream.");
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return -1;
    }
    return descriptor->takeFileDescriptor();
}

bool sharedMicrogUidForPackage(const QString &directory, const QString &package, int *uid, QString *error)
{
    if (error) error->clear();
    if (uid) *uid = -1;
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    static const QRegularExpression packagePattern(QStringLiteral(
        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    if (!uid || package.size() > 255 || !packagePattern.match(package).hasMatch() ||
        directory.isEmpty() || !QFileInfo(directory).isDir() || QFileInfo(directory).isSymLink())
        return fail(QStringLiteral("Invalid UID registry directory or package."));
    const QString path = QDir(directory).filePath(QStringLiteral("shared-microg-uids.json"));
    const QString lockPath = path + QStringLiteral(".lock");
    if (QFileInfo(path).isSymLink() || QFileInfo(lockPath).isSymLink())
        return fail(QStringLiteral("UID registry must not be a symbolic link."));
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) return fail(QStringLiteral("Cannot lock UID registry."));
    if (QFileInfo(path).isSymLink()) return fail(QStringLiteral("UID registry must not be a symbolic link."));
    QJsonObject entries;
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            return fail(QStringLiteral("Cannot read UID registry."));
        QJsonParseError parse;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
        const QJsonObject root = doc.object();
        if (file.error() != QFile::NoError || parse.error != QJsonParseError::NoError ||
            !doc.isObject() || root.size() != 2 || root.value(QStringLiteral("version")) != QJsonValue(1) ||
            !root.value(QStringLiteral("packages")).isObject())
            return fail(QStringLiteral("Invalid UID registry."));
        entries = root.value(QStringLiteral("packages")).toObject();
    }
    QSet<int> used;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const int value = it.value().toInt(-1);
        if (it.key().size() > 255 || !packagePattern.match(it.key()).hasMatch() ||
            !it.value().isDouble() || it.value().toDouble() != value ||
            value < 11000 || value > 19999 || used.contains(value))
            return fail(QStringLiteral("Invalid UID registry entry."));
        used.insert(value);
    }
    if (entries.contains(package)) {
        *uid = entries.value(package).toInt();
        return true;
    }
    int value = 11000;
    while (value <= 19999 && used.contains(value)) ++value;
    if (value > 19999) return fail(QStringLiteral("UID registry exhausted."));
    entries.insert(package, value);
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("packages"), entries);
    const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (contents.size() > 1024 * 1024) return fail(QStringLiteral("UID registry is too large."));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(contents) != contents.size() || !output.commit())
        return fail(QStringLiteral("Cannot save UID registry."));
    *uid = value;
    return true;
}

