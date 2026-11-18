#include "network_host.h"
#include "network_snapshot.h"

#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QThread>

namespace {
int transport(QNetworkConfiguration::BearerType bearer)
{
    switch (bearer) {
    case QNetworkConfiguration::BearerWLAN: return MEGLER_NETWORK_TRANSPORT_WIFI;
    case QNetworkConfiguration::BearerEthernet: return MEGLER_NETWORK_TRANSPORT_ETHERNET;
    case QNetworkConfiguration::BearerBluetooth: return MEGLER_NETWORK_TRANSPORT_BLUETOOTH;
    case QNetworkConfiguration::Bearer2G:
    case QNetworkConfiguration::BearerCDMA2000:
    case QNetworkConfiguration::BearerWCDMA:
    case QNetworkConfiguration::BearerHSPA:
    case QNetworkConfiguration::BearerEVDO:
    case QNetworkConfiguration::BearerLTE:
    case QNetworkConfiguration::Bearer3G:
    case QNetworkConfiguration::Bearer4G: return MEGLER_NETWORK_TRANSPORT_CELLULAR;
    default: return 0;
    }
}

int connmanTransport(const QString &type)
{
    if (type == QLatin1String("wifi")) return MEGLER_NETWORK_TRANSPORT_WIFI;
    if (type == QLatin1String("cellular")) return MEGLER_NETWORK_TRANSPORT_CELLULAR;
    if (type == QLatin1String("ethernet")) return MEGLER_NETWORK_TRANSPORT_ETHERNET;
    if (type == QLatin1String("bluetooth")) return MEGLER_NETWORK_TRANSPORT_BLUETOOTH;
    return 0;
}
}

AuroraNetworkHost::AuroraNetworkHost(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<ConnmanService>();
    qDBusRegisterMetaType<ConnmanServices>();
    connect(&m_manager, &QNetworkConfigurationManager::onlineStateChanged,
            this, [this](bool) { invalidateRoute(); });
    connect(&m_manager, &QNetworkConfigurationManager::configurationAdded,
            this, [this](const QNetworkConfiguration &) { invalidateRoute(); });
    connect(&m_manager, &QNetworkConfigurationManager::configurationRemoved,
            this, [this](const QNetworkConfiguration &) { invalidateRoute(); });
    connect(&m_manager, &QNetworkConfigurationManager::configurationChanged,
            this, [this](const QNetworkConfiguration &) { invalidateRoute(); });
    connect(&m_routeTimer, &QTimer::timeout, this, [this]() { requestRoute(); });
    m_routeTimer.start(2000);
    refresh();
    requestRoute();
}

AuroraNetworkHost::~AuroraNetworkHost() { shutdown(); }

void AuroraNetworkHost::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopped) return;
    m_stopped = true;
    m_routeTimer.stop();
    megler_network_publish(MEGLER_NETWORK_UNKNOWN);
}

void AuroraNetworkHost::invalidateRoute()
{
    if (m_stopped) return;
    ++m_routeGeneration;
    m_route = ConnmanRoute();
    refresh();
    requestRoute();
}

void AuroraNetworkHost::requestRoute()
{
    if (m_stopped || m_routeRequestPending) return;
    m_routeRequestPending = true;
    const unsigned int generation = m_routeGeneration;
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("net.connman"), QStringLiteral("/"),
        QStringLiteral("net.connman.Manager"), QStringLiteral("GetServices"));
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(message, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<ConnmanServices> reply = *call;
        call->deleteLater();
        m_routeRequestPending = false;
        if (m_stopped) return;
        if (generation != m_routeGeneration) {
            requestRoute();
            return;
        }
        if (reply.isError()) {
            const QString error = reply.error().name();
            if (m_routeError != error)
                qWarning() << "Megler network: ConnMan GetServices failed:" << error;
            m_routeError = error;
            m_route = ConnmanRoute();
        } else {
            if (!m_routeError.isEmpty())
                qInfo() << "Megler network: ConnMan GetServices recovered";
            m_routeError.clear();
            m_route = connmanRoute(reply.value());
        }
        refresh();
    });
}

void AuroraNetworkHost::refresh()
{
    if (m_stopped) return;
    const QList<QNetworkConfiguration> configs = m_manager.allConfigurations();
    int active = 0;
    int activeTransport = 0;
    bool known = false;
    for (const QNetworkConfiguration &config : configs) {
        if (!config.isValid() || config.type() != QNetworkConfiguration::InternetAccessPoint) continue;
        known = true;
        if ((config.state() & QNetworkConfiguration::Active) != QNetworkConfiguration::Active) continue;
        ++active;
        activeTransport = transport(config.bearerType());
    }
    const bool online = m_manager.isOnline();
    const int snapshot = auroraNetworkCapabilities(
        auroraNetworkSnapshot(known, active, activeTransport, online),
        m_route.valid, m_route.valid, connmanTransport(m_route.type),
        m_route.online, m_route.favorite);
    if (snapshot != m_lastSnapshot) {
        qInfo() << "Megler network: snapshot" << snapshot << "active configurations" << active
                << "online probe" << online << "ConnMan unique connected route valid" << m_route.valid;
        m_lastSnapshot = snapshot;
    }
    megler_network_publish(snapshot);
}
