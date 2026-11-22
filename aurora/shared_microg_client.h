#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_CLIENT_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_CLIENT_H

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QString>
#include <QUuid>
#include <memory>

#include "shared_microg_ipc.h"
#include "manager_identity.h"

class SharedMicrogClientChannel
{
public:
    SharedMicrogClientChannel()
        : m_connectionName(QStringLiteral("shared-microg-client-") +
                           QUuid::createUuid().toString().mid(1, 36))
    {
    }

    SharedMicrogClientChannel(const SharedMicrogClientChannel &) = delete;
    SharedMicrogClientChannel &operator=(const SharedMicrogClientChannel &) = delete;

    ~SharedMicrogClientChannel() { close(); }

    // Use on one Qt thread. Reentrant close cancels open without dropping its pending D-Bus call.
    bool open(QString *error)
    {
        return openImpl(QString(), QString(), QByteArray(), error);
    }

    bool openClient(const QString &expectedPackage, QString *error)
    {
        if (expectedPackage.isEmpty()) {
            if (error) *error = QStringLiteral("The client package is required.");
            return false;
        }
        return openImpl(expectedPackage, QString(), QByteArray(), error);
    }

    bool openClient(const QString &expectedPackage, const QString &apkPath, QString *error)
    {
        if (expectedPackage.isEmpty() || apkPath.isEmpty()) {
            if (error) *error = QStringLiteral("The client package and APK path are required.");
            return false;
        }
        return openImpl(expectedPackage, apkPath, QByteArray(), error);
    }

    bool openClientWithMetadata(const QString &expectedPackage, const QByteArray &metadata,
                                QString *error)
    {
        if (expectedPackage.isEmpty() || metadata.isEmpty() || metadata.size() > 1024 * 1024) {
            if (error) *error = QStringLiteral("The client package and bounded APK metadata are required.");
            return false;
        }
        return openImpl(expectedPackage, QString(), metadata, error);
    }

    int uid() const { return m_uid; }
    int pid() const { return m_pid; }
    QString package() const { return m_package; }

private:
    bool openImpl(const QString &expectedPackage, const QString &apkPath,
                  const QByteArray &metadata, QString *error)
    {
        if (error) error->clear();
        if (m_opening || m_opened || m_closed) {
            if (error) *error = QStringLiteral("The manager channel has already been opened or closed.");
            return false;
        }
        m_bus.reset(new QDBusConnection(QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, m_connectionName)));
        if (!m_bus || !m_bus->isConnected()) {
            if (error) *error = QStringLiteral("The manager D-Bus connection is unavailable.");
            close();
            return false;
        }

        QDBusMessage request = QDBusMessage::createMethodCall(
            QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
            QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
            QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
            expectedPackage.isEmpty() ? QStringLiteral("OpenChannel")
                : !metadata.isEmpty() ? QStringLiteral("OpenClientChannelWithMetadata")
                : apkPath.isEmpty() ? QStringLiteral("OpenClientChannel")
                                    : QStringLiteral("OpenClientChannelWithApk"));
        if (!apkPath.isEmpty()) request << apkPath;
        if (!metadata.isEmpty()) request << metadata;
        request.setAutoStartService(true);
        QDBusUnixFileDescriptor descriptor;
        QString requestError;
        int uid = 0, pid = 0;
        QString package, managerSender;
        m_opening = true;
        {
            std::unique_ptr<QDBusPendingCallWatcher> watcher(
                new QDBusPendingCallWatcher(m_bus->asyncCall(request, 15000)));
            auto waitForCall = [&]() {
                if (!watcher->isFinished()) {
                    QEventLoop loop;
                    QObject::connect(watcher.get(), &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
                    loop.exec();
                }
            };
            waitForCall();
            if (!m_closed && (!apkPath.isEmpty() || !metadata.isEmpty()) && watcher->isFinished() &&
                watcher->reply().type() == QDBusMessage::ErrorMessage &&
                (watcher->reply().errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod") ||
                 watcher->reply().errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface"))) {
                QDBusMessage legacyRequest = QDBusMessage::createMethodCall(
                    QStringLiteral("ru.androidcompatibilitylayer.appmanager"),
                    QStringLiteral("/ru/androidcompatibilitylayer/appmanager/microg"),
                    QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg"),
                    QStringLiteral("OpenClientChannel"));
                legacyRequest.setAutoStartService(true);
                watcher.reset(new QDBusPendingCallWatcher(m_bus->asyncCall(legacyRequest, 15000)));
                waitForCall();
            }
            m_opening = false;
            if (m_closed) {
                if (error) *error = QStringLiteral("The manager channel request was cancelled.");
                dispose();
                return false;
            }
            if (!watcher->isFinished()) {
                requestError = QStringLiteral("The manager channel request timed out.");
            } else {
                managerSender = watcher->reply().service();
                if (expectedPackage.isEmpty()) {
                    QDBusPendingReply<QDBusUnixFileDescriptor> reply(*watcher);
                    if (reply.isError())
                        requestError = reply.error().name() + QStringLiteral(": ") + reply.error().message();
                    else
                        descriptor = reply.value();
                } else {
                    QDBusPendingReply<QDBusUnixFileDescriptor, int, int, QString> reply(*watcher);
                    if (reply.isError())
                        requestError = reply.error().name() + QStringLiteral(": ") + reply.error().message();
                    else {
                        descriptor = reply.argumentAt<0>();
                        uid = reply.argumentAt<1>();
                        pid = reply.argumentAt<2>();
                        package = reply.argumentAt<3>();
                        if (uid < 11000 || uid > 19999 || pid < 50000 || package != expectedPackage ||
                            !managerSender.startsWith(QLatin1Char(':')))
                            requestError = QStringLiteral("The manager returned an invalid client identity.");
                    }
                }
            }
        }
        if (!expectedPackage.isEmpty() && requestError.isEmpty() && !m_closed) {
            QDBusMessage identify = QDBusMessage::createMethodCall(QStringLiteral("ru.omp.am"),
                QStringLiteral("/ru/omp/am"), QStringLiteral("ru.omp.am.Identify"),
                QStringLiteral("IdentifyByBusName"));
            identify << managerSender;
            QDBusPendingCallWatcher watcher(m_bus->asyncCall(identify, 5000));
            if (!watcher.isFinished()) {
                QEventLoop loop;
                QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
                loop.exec();
            }
            QDBusPendingReply<QString, QString> identity(watcher);
            if (identity.isError()) {
                requestError = QStringLiteral("Manager identity could not be verified: %1: %2")
                    .arg(identity.error().name(), identity.error().message());
            } else if (!isTrustedAppManagerExecutable(identity.argumentAt<0>()) ||
                       identity.argumentAt<1>().isEmpty() ||
                       identity.argumentAt<1>().size() > MaxAppManagerIdentityKeyLength) {
                requestError = QStringLiteral("Manager identity could not be verified.");
            }
        }
        if (!requestError.isEmpty()) {
            if (error) *error = requestError;
            close();
            return false;
        }
        if (!descriptor.isValid()) {
            if (error) *error = QStringLiteral("The manager returned an invalid channel descriptor.");
            close();
            return false;
        }
        m_descriptor = descriptor;
        m_uid = uid;
        m_pid = pid;
        m_package = package;
        m_opened = true;
        return true;
    }

public:
    int takeStream(QString *error)
    {
        if (error) error->clear();
        if (!m_opened || !m_descriptor.isValid()) {
            if (error) *error = QStringLiteral("No manager channel descriptor is available.");
            return -1;
        }
        return takeSharedMicrogStream(&m_descriptor, error);
    }

    void close()
    {
        m_closed = true;
        if (!m_opening) dispose();
    }

private:
    void dispose()
    {
        m_descriptor = QDBusUnixFileDescriptor();
        m_bus.reset();
        QDBusConnection::disconnectFromBus(m_connectionName);
    }

    QString m_connectionName;
    std::unique_ptr<QDBusConnection> m_bus;
    QDBusUnixFileDescriptor m_descriptor;
    bool m_opened = false;
    bool m_opening = false;
    bool m_closed = false;
    int m_uid = 0;
    int m_pid = 0;
    QString m_package;
};

#endif
