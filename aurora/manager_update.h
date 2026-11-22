#ifndef ANDROID_COMPATIBILITY_LAYER_MANAGER_UPDATE_H
#define ANDROID_COMPATIBILITY_LAYER_MANAGER_UPDATE_H

#include "package_architecture.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

inline QString managerUpdatePackageArchitecture()
{
    const QString architecture = packageBuildArchitecture();
    if (architecture == QLatin1String("aarch64") || architecture == QLatin1String("armv7hl"))
        return architecture;
    return QString();
}

struct ManagerUpdateRelease {
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
    QString version;
};

enum class ManagerUpdateResult { Update, Current, Error };

ManagerUpdateResult managerUpdateRelease(const QByteArray &json, const QString &installedVersion,
                                        const QString &architecture, ManagerUpdateRelease *release,
                                        QString *error);
bool managerUpdateUrlAllowed(const QUrl &url);

class ManagerUpdate : public QObject
{
    Q_OBJECT
public:
    explicit ManagerUpdate(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    bool start(const QString &directory, const QString &version, const QString &architecture, QString *error,
               bool checkOnly = false);
    bool busy() const { return m_busy; }

signals:
    void progress(int percent);
    void current();
    void available(const QString &version);
    void ready(const QString &path, const QString &version);
    void failed(const QString &message);

private:
    void request(const QUrl &url);
    void readReply();
    void replyFinished();
    void fail(const QString &message);
    void stopReply();

    QNetworkAccessManager m_network;
    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_reply = nullptr;
    QScopedPointer<QTemporaryFile> m_file;
    QCryptographicHash m_hash;
    QTimer m_timeout;
    ManagerUpdateRelease m_release;
    QByteArray m_metadata;
    QString m_directory;
    QString m_version;
    QString m_architecture;
    qint64 m_received = 0;
    int m_redirects = 0;
    int m_lastProgress = -1;
    bool m_busy = false;
    bool m_downloading = false;
    bool m_checkOnly = false;
};

#endif
