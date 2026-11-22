#include "notification_host.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QMutexLocker>
#include <QVariant>

#include <cstring>

namespace {
const int TimeoutMs = 5000;
const int MaxNotifications = 256;
const int OnlyAlertOnce = 8;

bool bounded(const char *text, size_t max)
{
    return !text || strnlen(text, max + 1) <= max;
}

QDBusMessage call(const QDBusMessage &message)
{
    return QDBusConnection::sessionBus().call(message, QDBus::Block, TimeoutMs);
}

QString notificationOwner()
{
    QDBusMessage query = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query << QStringLiteral("org.freedesktop.Notifications");
    const QDBusMessage reply = call(query);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return QString();
    return reply.arguments().first().toString();
}

QDBusMessage method(const QString &owner, const QString &name)
{
    return QDBusMessage::createMethodCall(owner, QStringLiteral("/org/freedesktop/Notifications"),
                                          QStringLiteral("org.freedesktop.Notifications"), name);
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
                                              int progress, bool indeterminate)
{
    return user && static_cast<NotificationHost *>(user)->process(MEGLER_NOTIFICATION_POST, package, tag, id,
                                                                   title, body, flags, max, progress, indeterminate);
}

bool NotificationHost::process(int operation, const char *package, const char *tag, int id,
                               const char *title, const char *body, int flags, int max,
                               int progress, bool indeterminate)
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
    const QString owner = notificationOwner();
    if (owner.isEmpty()) {
        qWarning("System notification service is unavailable.");
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
        if (hasProgress)
            hints.insert(QStringLiteral("x-nemo-progress"),
                         indeterminate ? -1.0 : qBound(0.0, double(progress) / double(max), 1.0));
        if (current != m_ids.end() && (hasProgress || (flags & OnlyAlertOnce))) {
            hints.insert(QStringLiteral("x-nemo-preview-summary"), QString());
            hints.insert(QStringLiteral("x-nemo-preview-body"), QString());
            hints.insert(QStringLiteral("suppress-sound"), true);
        }
        QDBusMessage message = method(owner, QStringLiteral("Notify"));
        message << app << (current == m_ids.end() ? uint(0) : current.value()) << QString()
                << QString::fromUtf8(title ? title : "") << QString::fromUtf8(body ? body : "")
                << QStringList() << hints << int(hasProgress ? 0 : -1);
        const QDBusMessage reply = call(message);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1 ||
            !reply.arguments().first().toUInt()) {
            qWarning("System notification publication failed.");
            return false;
        }
        m_ids.insert(key, reply.arguments().first().toUInt());
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
        } else {
            qWarning("System notification cancellation failed.");
            success = false;
            ++it;
        }
        if (operation != MEGLER_NOTIFICATION_CANCEL_ALL) break;
    }
    return success;
}
