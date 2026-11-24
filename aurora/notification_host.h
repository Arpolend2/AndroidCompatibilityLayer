#ifndef ANDROID_COMPATIBILITY_LAYER_NOTIFICATION_HOST_H
#define ANDROID_COMPATIBILITY_LAYER_NOTIFICATION_HOST_H

#include "../core/include/megler.h"

#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>

class QDBusMessage;

class NotificationHost : public QObject
{
    Q_OBJECT
public:
    explicit NotificationHost(QObject *parent = nullptr);
    megler_notification_host host();

private slots:
    void notificationClosed(uint id, uint reason, const QDBusMessage &message);

private:
    static bool MEGLER_CB send(void *user, int operation, const char *package,
                               const char *tag, int id, const char *title, const char *body);
    static bool MEGLER_CB sendDetailed(void *user, const char *package, const char *tag, int id,
                                       const char *title, const char *body, int flags, int max,
                                       int progress, bool indeterminate, int importance, bool silent);
    bool process(int operation, const char *package, const char *tag, int id,
                 const char *title, const char *body, int flags = 0, int max = 0,
                 int progress = 0, bool indeterminate = false, int importance = -1,
                 bool silent = false);

    QMutex m_mutex;
    QString m_package;
    QString m_owner;
    QMap<QPair<QString, int>, uint> m_ids;
};

#endif
