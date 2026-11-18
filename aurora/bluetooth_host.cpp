#include "bluetooth_host.h"

#include "bluetooth_advertisement.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <QThread>

#include <cstring>
#include <functional>

namespace {
const char *const bluezService = "org.bluez";
const char *const objectManager = "org.freedesktop.DBus.ObjectManager";
const char *const adapterInterface = "org.bluez.Adapter1";
const char *const deviceInterface = "org.bluez.Device1";
const int scanFailedInternalError = 3;

QVariant propertyValue(const QVariantMap &properties, const QString &name)
{
    QVariant value = properties.value(name);
    while (value.userType() == qMetaTypeId<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}

QString errorText(const QDBusMessage &message)
{
    return message.errorMessage().isEmpty() ? QStringLiteral("unknown D-Bus error") : message.errorMessage();
}

}

AuroraBluetoothHost::AuroraBluetoothHost(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::systemBus())
{
    connect(&m_pollTimer, &QTimer::timeout, this, &AuroraBluetoothHost::poll);
    connect(this, &AuroraBluetoothHost::scanRequested, this, &AuroraBluetoothHost::setScanMode,
            Qt::QueuedConnection);
    m_pollTimer.start(2000);
    poll();
}

megler_bluetooth_host AuroraBluetoothHost::callbacks()
{
    megler_bluetooth_host host = {};
    host.user = this;
    host.state = [](void *user, char *name, int size) {
        AuroraBluetoothHost *self = static_cast<AuroraBluetoothHost *>(user);
        QMutexLocker lock(&self->m_stateMutex);
        if (name && size > 0) {
            const QByteArray utf8 = self->m_adapterName.toUtf8();
            const int count = qMin(size - 1, utf8.size());
            std::memcpy(name, utf8.constData(), size_t(count));
            name[count] = 0;
        }
        return self->m_radioState;
    };
    host.scan = [](void *user, int mode) {
        AuroraBluetoothHost *self = static_cast<AuroraBluetoothHost *>(user);
        emit self->scanRequested(mode);
    };
    host.connect = [](void *, int conn, unsigned long long, int, bool) {
        qWarning() << "bluetooth: GATT connections are not supported by the Aurora BlueZ bridge";
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_CONNECTION;
        event.conn = conn;
        event.status = 257;
        megler_bluetooth_push(&event);
    };
    host.discover = [](void *, int conn) {
        qWarning() << "bluetooth: GATT service discovery is not supported by the Aurora BlueZ bridge";
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_SERVICES;
        event.conn = conn;
        event.status = 257;
        megler_bluetooth_push(&event);
    };
    host.read = [](void *, int conn, int handle) {
        qWarning() << "bluetooth: GATT reads are not supported by the Aurora BlueZ bridge";
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_READ;
        event.conn = conn;
        event.handle = handle;
        event.status = 257;
        megler_bluetooth_push(&event);
    };
    host.write = [](void *, int conn, int handle, const unsigned char *, int, int) {
        qWarning() << "bluetooth: GATT writes and notifications are not supported by the Aurora BlueZ bridge";
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_WRITTEN;
        event.conn = conn;
        event.handle = handle;
        event.status = 257;
        megler_bluetooth_push(&event);
    };
    host.mtu = [](void *, int conn, int) {
        qWarning() << "bluetooth: requesting a GATT MTU is not supported by the Aurora BlueZ bridge";
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_MTU;
        event.conn = conn;
        event.value = 23;
        event.status = 257;
        megler_bluetooth_push(&event);
    };
    host.settings = [](void *) {
        qWarning() << "bluetooth: opening Aurora Bluetooth settings is not supported by the bridge";
    };
    return host;
}

void AuroraBluetoothHost::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopped) return;
    m_stopped = true;
    m_pollTimer.stop();
    m_scanMode = MEGLER_BT_SCAN_OFF;
    stopDiscovery();
}

void AuroraBluetoothHost::setScanMode(int mode)
{
    if (m_stopped) return;
    if (mode < MEGLER_BT_SCAN_OFF || mode > MEGLER_BT_SCAN_ACTIVE) {
        qWarning() << "bluetooth: ignoring invalid scan mode" << mode;
        return;
    }
    if (mode == m_scanMode) return;
    m_scanMode = mode;
    m_lastAdvertisements.clear();
    m_discoveryFailed = false;
    m_pollTimer.start(mode == MEGLER_BT_SCAN_OFF ? 2000 : 1000);
    if (mode == MEGLER_BT_SCAN_OFF) {
        stopDiscovery();
        return;
    }
    if (m_radioState != MEGLER_BT_RADIO_ON || m_adapterPath.isEmpty()) {
        qWarning() << "bluetooth: scan requested while no powered LE adapter is available";
        pushScanFailure();
        return;
    }
    if (mode == MEGLER_BT_SCAN_ACTIVE)
        qInfo() << "bluetooth: BlueZ does not expose Android's active/passive scan distinction; using adapter discovery";
    startDiscovery();
}

void AuroraBluetoothHost::poll()
{
    if (m_stopped || m_pollPending || !m_bus.isConnected()) {
        if (!m_bus.isConnected() && !m_stopped &&
            m_lastPollError != QStringLiteral("system D-Bus is unavailable")) {
            qWarning() << "bluetooth: system D-Bus is unavailable";
            m_lastPollError = QStringLiteral("system D-Bus is unavailable");
        }
        return;
    }
    m_pollPending = true;
    readObjects();
}

void AuroraBluetoothHost::readObjects()
{
    asyncCall(QStringLiteral("/"), QString::fromLatin1(objectManager), QStringLiteral("GetManagedObjects"),
              QVariantList(), [this](const QDBusMessage &reply) {
        m_pollPending = false;
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            const QString error = errorText(reply);
            if (m_lastPollError != error) {
                qWarning() << "bluetooth: cannot read BlueZ objects:" << error;
                m_lastPollError = error;
            }
            if (m_radioState != MEGLER_BT_RADIO_NONE) updateObjects(ManagedObjects());
            return;
        }
        m_lastPollError.clear();
        const QVariant value = reply.arguments().first();
        ManagedObjects objects;
        if (!AuroraBluetooth::readManagedObjects(value, &objects)) {
            qWarning() << "bluetooth: BlueZ returned an invalid object snapshot";
            return;
        }
        updateObjects(objects);
    });
}

void AuroraBluetoothHost::updateObjects(const ManagedObjects &objects)
{
    QString adapterPath;
    QVariantMap adapterProperties;
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const auto found = it.value().constFind(QString::fromLatin1(adapterInterface));
        if (found == it.value().constEnd()) continue;
        adapterPath = it.key();
        adapterProperties = found.value();
        break;
    }

    const bool hasAdapter = !adapterPath.isEmpty();
    const int nextState = !hasAdapter ? MEGLER_BT_RADIO_NONE
        : propertyValue(adapterProperties, QStringLiteral("Powered")).toBool() ? MEGLER_BT_RADIO_ON
        : MEGLER_BT_RADIO_OFF;
    const QString nextName = hasAdapter ? propertyValue(adapterProperties, QStringLiteral("Name")).toString() : QString();
    bool stateChanged = false;
    int previousState = MEGLER_BT_RADIO_NONE;
    {
        QMutexLocker lock(&m_stateMutex);
        previousState = m_radioState;
        stateChanged = m_radioState != nextState || m_adapterName != nextName;
        m_radioState = nextState;
        m_adapterName = nextName;
    }
    if (previousState != MEGLER_BT_RADIO_ON && nextState == MEGLER_BT_RADIO_ON)
        m_discoveryFailed = false;
    m_adapterPath = adapterPath;
    if (stateChanged) {
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_STATE;
        event.value = nextState;
        const QByteArray name = nextName.toUtf8();
        event.name = name.isEmpty() ? nullptr : name.constData();
        megler_bluetooth_push(&event);
        qInfo() << "bluetooth: BlueZ adapter state" << nextState << nextName;
    }

    if (nextState != MEGLER_BT_RADIO_ON && m_discovering) stopDiscovery();
    if (m_scanMode != MEGLER_BT_SCAN_OFF && nextState == MEGLER_BT_RADIO_ON && !m_discovering &&
        !m_discoveryStarting && !m_discoveryFailed)
        startDiscovery();

    if (m_scanMode == MEGLER_BT_SCAN_OFF) return;
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const auto found = it.value().constFind(QString::fromLatin1(deviceInterface));
        if (found == it.value().constEnd()) continue;
        const QVariantMap &properties = found.value();
        const QString address = propertyValue(properties, QStringLiteral("Address")).toString();
        unsigned long long value = 0;
        if (!AuroraBluetooth::addressValue(address, &value)) continue;
        bool hasRssi = false;
        const int rssi = propertyValue(properties, QStringLiteral("RSSI")).toInt(&hasRssi);
        if (!hasRssi) continue;
        const QString name = propertyValue(properties, QStringLiteral("Name")).toString();
        const QStringList uuids = propertyValue(properties, QStringLiteral("UUIDs")).toStringList();
        bool hasTxPower = false;
        const int txPower = propertyValue(properties, QStringLiteral("TxPower")).toInt(&hasTxPower);
        const QByteArray data = AuroraBluetooth::advertisementData(name, uuids, hasTxPower ? txPower : 128);
        QByteArray fingerprint = data;
        fingerprint.append(char(rssi));
        const int addressType = AuroraBluetooth::addressType(
            propertyValue(properties, QStringLiteral("AddressType")).toString());
        fingerprint.append(char(addressType));
        if (m_lastAdvertisements.value(value) == fingerprint && m_lastAdvertisements.contains(value)) continue;
        m_lastAdvertisements.insert(value, fingerprint);
        megler_bluetooth_event event = {};
        event.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
        event.address = value;
        event.address_type = addressType;
        event.value = rssi;
        event.extra = 0;
        event.data = reinterpret_cast<const unsigned char *>(data.constData());
        event.size = data.size();
        const QByteArray nameUtf8 = name.toUtf8();
        event.name = nameUtf8.isEmpty() ? nullptr : nameUtf8.constData();
        event.time_ns = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() * 1000000LL;
        megler_bluetooth_push(&event);
    }
}

void AuroraBluetoothHost::startDiscovery()
{
    if (m_stopped || m_discoveryStarting || m_discovering || m_adapterPath.isEmpty()) return;
    m_discoveryStarting = true;
    QVariantMap filter;
    filter.insert(QStringLiteral("Transport"), QStringLiteral("le"));
    filter.insert(QStringLiteral("DuplicateData"), true);
    asyncCall(m_adapterPath, QString::fromLatin1(adapterInterface), QStringLiteral("SetDiscoveryFilter"),
              QVariantList() << QVariant::fromValue(filter), [this](const QDBusMessage &filterReply) {
        if (filterReply.type() == QDBusMessage::ErrorMessage) {
            m_discoveryStarting = false;
            m_discoveryFailed = true;
            qWarning() << "bluetooth: cannot set the LE scan filter:" << errorText(filterReply);
            pushScanFailure();
            return;
        }
        asyncCall(m_adapterPath, QString::fromLatin1(adapterInterface), QStringLiteral("StartDiscovery"),
                  QVariantList(), [this](const QDBusMessage &reply) {
            m_discoveryStarting = false;
            if (reply.type() == QDBusMessage::ErrorMessage) {
                m_discoveryFailed = true;
                qWarning() << "bluetooth: cannot start LE discovery:" << errorText(reply);
                pushScanFailure();
                return;
            }
            m_discoveryFailed = false;
            m_discovering = true;
            qInfo() << "bluetooth: LE discovery started";
            if (m_scanMode == MEGLER_BT_SCAN_OFF) stopDiscovery();
        });
    });
}

void AuroraBluetoothHost::stopDiscovery()
{
    if (!m_discovering || m_adapterPath.isEmpty()) return;
    m_discovering = false;
    asyncCall(m_adapterPath, QString::fromLatin1(adapterInterface), QStringLiteral("StopDiscovery"),
              QVariantList(), [this](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage)
            qWarning() << "bluetooth: cannot stop LE discovery:" << errorText(reply);
        else
            qInfo() << "bluetooth: LE discovery stopped";
    });
}

void AuroraBluetoothHost::asyncCall(const QString &path, const QString &interface, const QString &method,
                                    const QVariantList &arguments,
                                    const std::function<void(const QDBusMessage &)> &finished)
{
    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromLatin1(bluezService), path, interface, method);
    request.setArguments(arguments);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [finished](QDBusPendingCallWatcher *call) {
        const QDBusMessage reply = call->reply();
        call->deleteLater();
        finished(reply);
    });
}

void AuroraBluetoothHost::pushScanFailure()
{
    megler_bluetooth_event event = {};
    event.kind = MEGLER_BT_EVENT_SCAN_FAILED;
    event.status = scanFailedInternalError;
    megler_bluetooth_push(&event);
}

namespace {
AuroraBluetoothHost &host()
{
    static AuroraBluetoothHost instance;
    return instance;
}
}

megler_bluetooth_host auroraBluetoothHost()
{
    if (!QCoreApplication::instance()) qFatal("bluetooth: QCoreApplication is required");
    return host().callbacks();
}

void auroraBluetoothShutdown()
{
    host().shutdown();
}

