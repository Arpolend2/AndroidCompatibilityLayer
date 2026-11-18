#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_HOST_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_HOST_H

#include "install_request_channel.h"
#include "../core/include/megler.h"
#include <QMap>

// Keep alive until Megler stops invoking the system callbacks.
class InstallRequestHost : public QObject
{
    Q_OBJECT
public:
    InstallRequestHost(const QString &endpoint, const QString &resultsDir,
                       const megler_system_host &other, QObject *parent = nullptr);
    megler_system_host host();
    QString appsDir() const;
    QString error() const { return m_error; }

private:
    static void MEGLER_CB openUrl(void *, const char *);
    static void MEGLER_CB shareText(void *, const char *, const char *);
    static bool MEGLER_CB install(void *, const char *, const char *);
    static bool MEGLER_CB launch(void *, const char *, const char *);
    static bool MEGLER_CB uninstall(void *, const char *);
    static int MEGLER_CB permission(void *, const char *, bool);
    static bool MEGLER_CB pick(void *, const megler_pick_request *);
    void completed(const QString &outcome, const QString &message,
                   const QString &package, const QString &label, quint32 version);

    QString m_directory;
    InstallRequestClient m_client;
    megler_system_host m_other;
    QString m_error;
    QMutex m_mutex;
    QString m_pendingName;
    QMap<QString, QString> m_apps;
    unsigned m_resultCount = 0;
};

#endif
