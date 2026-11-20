#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_STANDALONE_APK_EXPORT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_STANDALONE_APK_EXPORT_H

#include "../core/include/megler.h"
#include <QObject>
#include <QMutex>
#include <QSet>
#include <QTemporaryDir>
#include <atomic>
#include <memory>

struct StandaloneLaunchResult;
typedef std::shared_ptr<StandaloneLaunchResult> StandaloneLaunchResultPtr;
Q_DECLARE_METATYPE(StandaloneLaunchResultPtr)

class StandaloneApkExport : public QObject
{
    Q_OBJECT
public:
    enum LaunchMode { WaitForLaunchResult, QueueLaunch };
    StandaloneApkExport(const QString &dataRoot, const QString &sharedRoot,
                        const megler_system_host &other, QObject *parent = nullptr,
                        const QString &libraryCachePath = QString(),
                        LaunchMode launchMode = WaitForLaunchResult);
    megler_system_host host();
    QString appsDir() const;
    QString error() const { return m_error; }
    static QString takeNext(const QString &sharedRoot, QString *error);
    static bool removeClaimed(const QString &sharedRoot, const QString &path, QString *error);

signals:
    void requested(const QString &path, const QString &name);
    void launchRequested(const QString &package, const StandaloneLaunchResultPtr &state);

private:
    static void MEGLER_CB openUrl(void *, const char *);
    static void MEGLER_CB shareText(void *, const char *, const char *);
    static bool MEGLER_CB install(void *, const char *, const char *);
    static bool MEGLER_CB launch(void *, const char *, const char *);
    static bool MEGLER_CB uninstall(void *, const char *);
    static int MEGLER_CB permission(void *, const char *, bool);
    static bool MEGLER_CB pick(void *, const megler_pick_request *);
    void processRequest(const QString &path, const QString &name);
    void processLaunch(const QString &package, const StandaloneLaunchResultPtr &state);
    void reportFailure(const QString &name, const QString &message);
    void reportResult(const QString &name, bool success, const QString &message);
    void refreshLibrary();

    QString m_dataRoot;
    QString m_sharedRoot;
    QString m_libraryCachePath;
    megler_system_host m_other;
    const LaunchMode m_launchMode;
    std::atomic<bool> m_launchPending{false};
    QTemporaryDir m_directory;
    QString m_error;
    QMutex m_mutex;
    bool m_busy = false;
    QSet<QString> m_pendingRequests;
    bool m_libraryPending = false;
    bool m_libraryWarned = false;
};

#endif
