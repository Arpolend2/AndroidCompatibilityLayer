#ifndef MEGLER_AURORA_STANDALONE_APK_EXPORT_H
#define MEGLER_AURORA_STANDALONE_APK_EXPORT_H

#include "../core/include/megler.h"
#include <QObject>
#include <QMutex>
#include <QTemporaryDir>

class StandaloneApkExport : public QObject
{
    Q_OBJECT
public:
    StandaloneApkExport(const QString &dataRoot, const megler_system_host &other, QObject *parent = nullptr);
    megler_system_host host();
    QString appsDir() const;
    QString error() const { return m_error; }

signals:
    void requested(const QString &path, const QString &name);

private:
    static void MEGLER_CB openUrl(void *, const char *);
    static void MEGLER_CB shareText(void *, const char *, const char *);
    static bool MEGLER_CB install(void *, const char *, const char *);
    static bool MEGLER_CB launch(void *, const char *, const char *);
    static bool MEGLER_CB uninstall(void *, const char *);
    static int MEGLER_CB permission(void *, const char *, bool);
    static bool MEGLER_CB pick(void *, const megler_pick_request *);
    void processRequest(const QString &path, const QString &name);
    void reportFailure(const QString &name, const QString &message);

    QString m_dataRoot;
    megler_system_host m_other;
    QTemporaryDir m_directory;
    QString m_error;
    QMutex m_mutex;
    bool m_busy = false;
};

#endif
