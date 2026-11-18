#include "external_apk_receiver.h"

#include "external_apk_path.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QJsonObject>
#include <RuntimeManager/RuntimeDispatcher>

namespace {
const char ServiceName[] = "ru.megler.MeglerProbe";
const char ObjectPath[] = "/ru/megler/MeglerProbe";
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

    auto *dispatcher = RuntimeManager::RuntimeDispatcher::instance();
    dispatcher->registerIntent(QStringLiteral("OpenURI"),
        [this](const QJsonObject &params, const RuntimeManager::RuntimeDispatcher::HandlerCb &reply) {
            const QJsonValue uri = params.value(QStringLiteral("uri"));
            if (uri.isString()) submit(QStringList() << uri.toString());
            else emit rejected(tr("The file request does not contain a valid URI."));
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

void ExternalApkReceiver::submit(const QStringList &arguments)
{
    if (arguments.size() != 1 || arguments.first().size() > 4096) {
        emit rejected(tr("Select exactly one APK or bundle file."));
        return;
    }
    QString path, error;
    if (!externalApkPath(arguments.first(), &path, &error, true)) {
        emit rejected(error);
        return;
    }
    emit requested(path);
}

void ExternalApkReceiver::openFile(const QStringList &arguments)
{
    submit(arguments);
}
