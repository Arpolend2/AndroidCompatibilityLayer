#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_OFFICIAL_STORE_DOWNLOAD_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_OFFICIAL_STORE_DOWNLOAD_H

#include <QObject>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

class OfficialStoreDownload : public QObject
{
    Q_OBJECT
public:
    explicit OfficialStoreDownload(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    bool start(const QString &cacheDirectory, QString *error);
    bool busy() const { return m_reply != nullptr; }
    void clear();

signals:
    void ready(const QString &path);
    void failed(const QString &message);
    void downloadProgress(int percent);

private:
    enum Phase { Metadata, Apk };
    void request(const QUrl &url);
    void readAvailable();
    void finished();
    void fail(const QString &message);

    QNetworkAccessManager m_network;
    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_reply;
    QTimer m_timeout;
    QScopedPointer<QTemporaryFile> m_file;
    QByteArray m_metadata;
    QUrl m_apkUrl;
    QString m_cacheDirectory;
    qint64 m_received;
    int m_lastProgress = -2;
    int m_redirects;
    Phase m_phase;
};

#endif
