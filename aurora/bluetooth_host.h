#pragma once

#include "../core/include/megler.h"

#include "bluetooth_dbus.h"

#include <QDBusConnection>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class AuroraBluetoothHost : public QObject {
    Q_OBJECT
public:
    using InterfaceProperties = AuroraBluetooth::InterfaceProperties;
    using ManagedObjects = AuroraBluetooth::ManagedObjects;

    explicit AuroraBluetoothHost(QObject *parent = nullptr);
    megler_bluetooth_host callbacks();
    void shutdown();

signals:
    void scanRequested(int mode);

private slots:
    void setScanMode(int mode);
    void poll();

private:
    void readObjects();
    void updateObjects(const ManagedObjects &objects);
    void startDiscovery();
    void stopDiscovery();
    void asyncCall(const QString &path, const QString &interface, const QString &method,
                   const QVariantList &arguments,
                   const std::function<void(const QDBusMessage &)> &finished);
    void pushScanFailure();

    QDBusConnection m_bus;
    QTimer m_pollTimer;
    QMutex m_stateMutex;
    QString m_adapterPath;
    QString m_adapterName;
    QMap<unsigned long long, QByteArray> m_lastAdvertisements;
    int m_radioState = MEGLER_BT_RADIO_NONE;
    int m_scanMode = MEGLER_BT_SCAN_OFF;
    bool m_pollPending = false;
    bool m_discoveryStarting = false;
    bool m_discovering = false;
    bool m_discoveryFailed = false;
    bool m_stopped = false;
    QString m_lastPollError;
};

megler_bluetooth_host auroraBluetoothHost();
void auroraBluetoothShutdown();
