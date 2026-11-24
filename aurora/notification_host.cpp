#include "notification_host.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QVariant>

#include <cstring>

namespace {
const int TimeoutMs = 5000;
const int OwnerRecoveryTimeoutMs = 500;
const int OwnerRecoveryPollMs = 20;
const int MaxNotifications = 256;
const int OnlyAlertOnce = 8;
const int Ongoing = 2 | 32 | 64;

bool bounded(const char *text, size_t max)
{
    return !text || strnlen(text, max + 1) <= max;
}

QDBusMessage call(const QDBusMessage &message)
{
    return QDBusConnection::sessionBus().call(message, QDBus::Block, TimeoutMs);
}

QString notificationOwner(QString *errorName = nullptr, QString *errorMessage = nullptr)
{
    QDBusMessage query = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query << QStringLiteral("org.freedesktop.Notifications");
    const QDBusMessage reply = call(query);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (errorName) *errorName = reply.errorName();
            if (errorMessage) *errorMessage = reply.errorMessage();
        } else {
            if (errorName) errorName->clear();
            if (errorMessage) errorMessage->clear();
        }
        return QString();
    }
    if (errorName) errorName->clear();
    if (errorMessage) errorMessage->clear();
    return reply.arguments().first().toString();
}

QString replacementNotificationOwner(const QString &previousOwner)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < OwnerRecoveryTimeoutMs) {
        const QString owner = notificationOwner();
        if (!owner.isEmpty() && owner != previousOwner) return owner;
        QThread::msleep(OwnerRecoveryPollMs);
    }
    return QString();
}

QDBusMessage method(const QString &owner, const QString &name)
{
    return QDBusMessage::createMethodCall(owner, QStringLiteral("/org/freedesktop/Notifications"),
                                          QStringLiteral("org.freedesktop.Notifications"), name);
}

bool ownerDisappeared(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ErrorMessage) return false;
    return reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown") ||
           reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner");
}

bool accessDenied(const QDBusMessage &reply)
{
    return reply.type() == QDBusMessage::ErrorMessage &&
           reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.AccessDenied");
}

QString singleLine(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.left(512);
}
}

NotificationHost::NotificationHost(QObject *parent) : QObject(parent)
{
    if (!QDBusConnection::sessionBus().connect(
            QStringLiteral("org.freedesktop.Notifications"),
            QStringLiteral("/org/freedesktop/Notifications"),
            QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("NotificationClosed"),
            this, SLOT(notificationClosed(uint,uint,QDBusMessage))))
        qWarning("Cannot subscribe to system notification closures.");
}

void NotificationHost::notificationClosed(uint id, uint, const QDBusMessage &message)
{
    QMutexLocker lock(&m_mutex);
    if (message.service() != m_owner) return;
    for (auto it = m_ids.begin(); it != m_ids.end();) {
        if (it.value() == id) it = m_ids.erase(it);
        else ++it;
    }
}

megler_notification_host NotificationHost::host()
{
    megler_notification_host result = {};
    result.user = this;
    result.send = send;
    result.send_detailed = sendDetailed;
    return result;
}

bool MEGLER_CB NotificationHost::send(void *user, int operation, const char *package,
                                      const char *tag, int id, const char *title, const char *body)
{
    return user && static_cast<NotificationHost *>(user)->process(operation, package, tag, id, title, body);
}

bool MEGLER_CB NotificationHost::sendDetailed(void *user, const char *package, const char *tag, int id,
                                              const char *title, const char *body, int flags, int max,
                                              int progress, bool indeterminate, int importance, bool silent)
{
    return user && static_cast<NotificationHost *>(user)->process(MEGLER_NOTIFICATION_POST, package, tag, id,
                                                                   title, body, flags, max, progress, indeterminate,
                                                                   importance, silent);
}

bool NotificationHost::process(int operation, const char *package, const char *tag, int id,
                               const char *title, const char *body, int flags, int max,
                               int progress, bool indeterminate, int importance, bool silent)
{
    if (!package || !*package || !bounded(package, 255) || !bounded(tag, 1024) ||
        !bounded(title, 4096) || !bounded(body, 16384) ||
        (operation != MEGLER_NOTIFICATION_POST && operation != MEGLER_NOTIFICATION_CANCEL &&
         operation != MEGLER_NOTIFICATION_CANCEL_ALL)) {
        qWarning("Invalid Android notification request.");
        return false;
    }

    QMutexLocker lock(&m_mutex);
    const QString app = QString::fromUtf8(package);
    if (!m_package.isEmpty() && m_package != app) {
        qWarning("Android notification package changed within the process.");
        return false;
    }
    m_package = app;
    QString ownerError;
    QString ownerMessage;
    QString owner = notificationOwner(&ownerError, &ownerMessage);
    if (owner.isEmpty()) {
        if (!ownerError.isEmpty()) {
            const QByteArray name = ownerError.toUtf8();
            const QByteArray message = singleLine(ownerMessage).toUtf8();
            if (ownerError == QLatin1String("org.freedesktop.DBus.Error.AccessDenied"))
                qWarning("System notification permission denied (D-Bus %s: %s).",
                         name.constData(), message.constData());
            else
                qWarning("System notification service is unavailable (D-Bus %s: %s).",
                         name.constData(), message.constData());
        } else {
            qWarning("System notification service is unavailable.");
        }
        return false;
    }
    if (owner != m_owner) {
        m_ids.clear();
        m_owner = owner;
    }
    const QPair<QString, int> key(tag ? QStringLiteral("1") + QString::fromUtf8(tag)
                                     : QStringLiteral("0"), id);
    if (operation == MEGLER_NOTIFICATION_POST) {
        auto current = m_ids.find(key);
        if (current == m_ids.end() && m_ids.size() >= MaxNotifications) {
            qWarning("Android notification limit reached.");
            return false;
        }
        const bool hasProgress = indeterminate || max > 0;
        QVariantMap hints;
        hints.insert(QStringLiteral("x-nemo-origin-package"), app);
        if (hasProgress)
            hints.insert(QStringLiteral("x-nemo-progress"),
                         indeterminate ? -1.0 : qBound(0.0, double(progress) / double(max), 1.0));
        const bool quiet = importance >= MEGLER_NOTIFICATION_IMPORTANCE_MIN &&
                           importance <= MEGLER_NOTIFICATION_IMPORTANCE_LOW;
        const bool noPreview = (importance >= MEGLER_NOTIFICATION_IMPORTANCE_MIN &&
                                importance <= MEGLER_NOTIFICATION_IMPORTANCE_DEFAULT) ||
                               (current != m_ids.end() && (hasProgress || (flags & OnlyAlertOnce)));
        if (importance >= MEGLER_NOTIFICATION_IMPORTANCE_MIN &&
            importance <= MEGLER_NOTIFICATION_IMPORTANCE_MAX)
            // Aurora Lipstick returns ID 0 for the spec's byte-typed urgency; it accepts int32.
            hints.insert(QStringLiteral("urgency"), quiet ? 0 : 1);
        if (noPreview) {
            hints.insert(QStringLiteral("x-nemo-preview-summary"), QString());
            hints.insert(QStringLiteral("x-nemo-preview-body"), QString());
        }
        if (silent || quiet || (current != m_ids.end() && (hasProgress || (flags & OnlyAlertOnce))))
            hints.insert(QStringLiteral("suppress-sound"), true);
        const auto makeMessage = [&](const QString &destination, uint replaces) {
            QDBusMessage message = method(destination, QStringLiteral("Notify"));
            message << app << replaces << QString()
                    << QString::fromUtf8(title ? title : "") << QString::fromUtf8(body ? body : "")
                    << QStringList() << hints << int(hasProgress || (flags & Ongoing) ? 0 : -1);
            return message;
        };
        const bool updating = current != m_ids.end();
        const uint replaces = updating ? current.value() : 0;
        bool isUpdate = updating;
        QDBusMessage reply = call(makeMessage(owner, replaces));
        if (ownerDisappeared(reply)) {
            const QString retryOwner = replacementNotificationOwner(owner);
            if (!retryOwner.isEmpty()) {
                owner = retryOwner;
                m_owner = retryOwner;
                m_ids.clear();
                isUpdate = false;
                reply = call(makeMessage(owner, 0));
            }
        }
        bool validId = false;
        const uint notificationId = reply.type() == QDBusMessage::ReplyMessage &&
                                    reply.arguments().size() == 1
            ? reply.arguments().first().toUInt(&validId) : 0;
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1 ||
            !validId || !notificationId) {
            const char *action = isUpdate ? "update" : "post";
            if (reply.type() == QDBusMessage::ErrorMessage) {
                const QByteArray name = reply.errorName().toUtf8();
                const QByteArray message = singleLine(reply.errorMessage()).toUtf8();
                if (ownerDisappeared(reply))
                    qWarning("System notification service is unavailable (D-Bus %s: %s).",
                             name.constData(), message.constData());
                else if (accessDenied(reply))
                    qWarning("System notification permission denied (D-Bus %s: %s).",
                             name.constData(), message.constData());
                else
                    qWarning("System notification %s failed (D-Bus %s: %s).",
                             action, name.constData(), message.constData());
            } else if (reply.type() != QDBusMessage::ReplyMessage) {
                qWarning("System notification %s failed: unexpected D-Bus reply type %d.",
                         action, int(reply.type()));
            } else if (reply.arguments().size() != 1) {
                qWarning("System notification %s failed: D-Bus reply had %d arguments; expected 1.",
                         action, reply.arguments().size());
            } else if (!validId) {
                const char *type = reply.arguments().first().typeName();
                qWarning("System notification %s failed: D-Bus notification ID had type %s.",
                         action, type ? type : "unknown");
            } else {
                qWarning("System notification %s failed: D-Bus returned notification ID 0.", action);
            }
            return false;
        }
        m_ids.insert(key, notificationId);
        if (!replaces) qInfo("ACL notification: posted");
        return true;
    }

    bool success = true;
    auto it = operation == MEGLER_NOTIFICATION_CANCEL_ALL ? m_ids.begin() : m_ids.find(key);
    while (it != m_ids.end()) {
        QDBusMessage message = method(owner, QStringLiteral("CloseNotification"));
        message << it.value();
        const QDBusMessage reply = call(message);
        if (reply.type() == QDBusMessage::ReplyMessage) {
            it = m_ids.erase(it);
            qInfo("ACL notification: removed");
        } else {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                const QByteArray name = reply.errorName().toUtf8();
                const QByteArray message = singleLine(reply.errorMessage()).toUtf8();
                if (ownerDisappeared(reply))
                    qWarning("System notification service is unavailable (D-Bus %s: %s).",
                             name.constData(), message.constData());
                else if (accessDenied(reply))
                    qWarning("System notification permission denied (D-Bus %s: %s).",
                             name.constData(), message.constData());
                else
                    qWarning("System notification cancellation failed (D-Bus %s: %s).",
                             name.constData(), message.constData());
            } else {
                qWarning("System notification cancellation failed: unexpected D-Bus reply type %d.",
                         int(reply.type()));
            }
            success = false;
            ++it;
        }
        if (operation != MEGLER_NOTIFICATION_CANCEL_ALL) break;
    }
    return success;
}
