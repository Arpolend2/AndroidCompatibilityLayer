#include "external_apk_receiver.h"

#include "external_apk_path.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDebug>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <RuntimeManager/RuntimeDispatcher>

namespace {
const char ServiceName[] = "ru.androidcompatibilitylayer.appmanager";
const char ObjectPath[] = "/ru/androidcompatibilitylayer/appmanager";
}

ExternalApkReceiver::ExternalApkReceiver(QObject *parent)
    : QObject(parent), m_legacyRegistered(false), m_runtimeStarted(false)
{
}

ExternalApkReceiver::~ExternalApkReceiver()
{
    auto *dispatcher = RuntimeManager::RuntimeDispatcher::instance();
    dispatcher->unregisterIntent(QStringLiteral("OpenURI"));
    if (m_runtimeStarted) dispatcher->stopService();
    if (m_legacyRegistered) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QString::fromLatin1(ObjectPath));
        bus.unregisterService(QString::fromLatin1(ServiceName));
    }
}

bool ExternalApkReceiver::start(QString *error)
{
    if (error) error->clear();
    if (m_legacyRegistered || m_runtimeStarted) {
        if (error) *error = tr("The file handler is already running.");
        return false;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    QString legacyError;
    if (!bus.registerObject(QString::fromLatin1(ObjectPath), this,
                            QDBusConnection::ExportScriptableSlots)) {
        legacyError = bus.lastError().message();
    } else if (!bus.registerService(QString::fromLatin1(ServiceName))) {
        legacyError = bus.lastError().message();
        bus.unregisterObject(QString::fromLatin1(ObjectPath));
    } else {
        m_legacyRegistered = true;
    }

    if (!m_legacyRegistered && bus.interface()) {
        const QDBusReply<QString> owner = bus.interface()->serviceOwner(QString::fromLatin1(ServiceName));
        if (owner.isValid() && owner.value() != bus.baseService()) {
            if (error) *error = tr("Another instance owns the file-open service.");
            return false;
        }
    }

    auto *dispatcher = RuntimeManager::RuntimeDispatcher::instance();
    dispatcher->registerIntent(QStringLiteral("OpenURI"),
        [this](const QJsonObject &params, const RuntimeManager::RuntimeDispatcher::HandlerCb &reply) {
            const QJsonValue uri = params.value(QStringLiteral("uri"));
            if (uri.isString()) submitFrom(QStringList() << uri.toString(), "runtime-open-uri");
            else {
                qInfo("APK request: source=runtime-open-uri rejected=missing-uri");
                emit rejected(tr("The file request does not contain a valid URI."));
            }
            reply(QJsonObject());
        });
    const RuntimeManager::Error runtimeError = dispatcher->startService();
    m_runtimeStarted = !runtimeError;
    if (!m_runtimeStarted) {
        dispatcher->unregisterIntent(QStringLiteral("OpenURI"));
        qWarning("RuntimeManager OpenURI service unavailable: %s", qPrintable(runtimeError.message()));
    }
    if (!m_legacyRegistered)
        qWarning("Legacy file-open service unavailable: %s", qPrintable(legacyError));
    if (!m_legacyRegistered && !m_runtimeStarted) {
        if (error) *error = tr("Unable to register the file-open service.");
        return false;
    }
    return true;
}

void ExternalApkReceiver::setActivationHandler(const std::function<bool()> &handler)
{
    m_activationHandler = handler;
}

bool ExternalApkReceiver::activate(const QStringList &arguments)
{
    if (arguments.isEmpty()) return m_activationHandler && m_activationHandler();
    return submitFrom(arguments, "activation");
}

bool ExternalApkReceiver::submit(const QStringList &arguments)
{
    return submitFrom(arguments, "direct");
}

bool ExternalApkReceiver::recordStoreResult(const QString &path, bool installed, const QByteArray &fingerprint)
{
    static const QRegularExpression name(QStringLiteral("^claimed_([0-9a-f]{32})\\.(apk|apks|xapk)$"));
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    const QRegularExpressionMatch match = name.match(QFileInfo(path).fileName());
    if (!match.hasMatch() || (installed && !hash.match(QString::fromLatin1(fingerprint)).hasMatch()))
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_storeResults.begin(); it != m_storeResults.end();) {
        if (it.value().time <= now - 30 * 60 * 1000) it = m_storeResults.erase(it);
        else ++it;
    }
    if (m_storeResults.size() >= 128 && !m_storeResults.contains(match.captured(1)))
        return false;
    m_storeResults.insert(match.captured(1),
                          {installed ? QStringLiteral("installed:") + QString::fromLatin1(fingerprint)
                                     : QStringLiteral("failed"), now, false});
    return true;
}

QString ExternalApkReceiver::storeResult(const QString &id)
{
    static const QRegularExpression valid(QStringLiteral("^[0-9a-f]{32}$"));
    if (!valid.match(id).hasMatch()) return QString();
    if (!m_storePollReported) {
        m_storePollReported = true;
        qInfo("APK handoff: manager received a store result query.");
    }
    auto it = m_storeResults.find(id);
    if (it == m_storeResults.end() ||
        it.value().time <= QDateTime::currentMSecsSinceEpoch() - 30 * 60 * 1000) return QString();
    if (!it.value().reported) {
        it.value().reported = true;
        qInfo("APK handoff: manager returned a recorded store result.");
    }
    return it.value().value;
}

QString ExternalApkReceiver::libraryApps()
{
    if (!m_libraryPollReported) {
        m_libraryPollReported = true;
        qInfo("APK handoff: manager received a library list query.");
    }
    return m_libraryApps;
}

QString ExternalApkReceiver::launchApp(const QString &package)
{
    static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    if (!calledFromDBus() || !m_launchHandler || package.size() > 159 ||
        !valid.match(package).hasMatch() || m_launchPending >= 16)
        return QStringLiteral("error:Invalid or unavailable app launch request.");
    setDelayedReply(true);
    const QDBusMessage request = message();
    const QDBusConnection bus = connection();
    ++m_launchPending;
    m_launchHandler(package, [this, bus, request](const QString &result) {
        --m_launchPending;
        if (!bus.send(request.createReply(QVariantList() << result)))
            qWarning("Cannot send the app launch result to the store.");
    });
    return QString();
}

bool ExternalApkReceiver::submitFrom(const QStringList &arguments, const char *source)
{
    qInfo("APK request: source=%s count=%d", source, arguments.size());
    if (arguments.size() != 1 || arguments.first().size() > 4096) {
        qInfo("APK request: rejected=argument-count-or-length");
        emit rejected(tr("Select exactly one APK or bundle file."));
        return false;
    }
    QString path, error;
    if (!externalApkPath(arguments.first(), &path, &error, true)) {
        emit rejected(error);
        return false;
    }
    emit requested(path);
    return true;
}

void ExternalApkReceiver::openFile(const QStringList &arguments)
{
    submitFrom(arguments, "legacy-open-file");
}
