#ifndef MEGLER_AURORA_MICROG_INSTALL_H
#define MEGLER_AURORA_MICROG_INSTALL_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSaveFile>
#include <QScopedPointer>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkReply;

struct MicrogRelease {
    QUrl url;
    QByteArray sha256;
    qint64 size = 0;
};

bool microgRelease(const QByteArray &json, MicrogRelease *release, QString *error);
bool microgDownloadUrlAllowed(const QUrl &url);

class MicrogInstall : public QObject
{
    Q_OBJECT
public:
    explicit MicrogInstall(const QString &destination, QObject *parent = nullptr);
    bool busy() const { return m_busy; }
    bool start(const QString &localPath, const std::function<bool()> &canPublish, QString *error);

signals:
    void finished(bool success, const QString &message);
    void downloadProgress(int percent);
    void validatingDownload();

private:
    enum Stage { Metadata, Download };
    void request(const QUrl &url);
    void readReply();
    void replyFinished();
    void beginPayload();
    void copyLocal();
    bool append(const QByteArray &bytes);
    void publish();
    void finish(bool success, const QString &message);

    QString m_destination;
    QString m_localPath;
    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    QScopedPointer<QSaveFile> m_output;
    QScopedPointer<QLockFile> m_lock;
    QFile m_input;
    QCryptographicHash m_hash;
    QTimer m_timeout;
    MicrogRelease m_release;
    QByteArray m_metadata;
    std::function<bool()> m_canPublish;
    Stage m_stage = Metadata;
    qint64 m_received = 0;
    int m_lastProgress = -2;
    int m_redirects = 0;
    bool m_busy = false;
};

#endif
