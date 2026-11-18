#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_CHANNEL_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_CHANNEL_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QTemporaryDir>

class QLocalServer;
class QLocalSocket;
class QProcess;

// One channel per preview process. All methods except submit() run on the
// object's thread. A request is only a handoff, never an installation result.
class InstallRequestServer : public QObject
{
    Q_OBJECT
public:
    explicit InstallRequestServer(QProcess *process, QObject *parent = nullptr);
    ~InstallRequestServer() override;
    bool listen(QString *error);
    QString endpoint() const;
    QString resultsDir() const;

    // Must be called only after the actual operation has completed.
    bool finish(quint64 id, const QString &outcome, const QString &message,
                const QString &package = QString(), const QString &label = QString(), quint32 version = 0);
    bool approve(quint64 id);

signals:
    void requested(quint64 id, const QString &path, const QString &name);
    void canceled(quint64 id);

private:
    void acceptPending();
    void readAvailable();
    void disconnected();
    void closeConnection();

    QProcess *m_process;
    QTemporaryDir m_directory;
    QLocalServer *m_server;
    QLocalSocket *m_socket;
    QByteArray m_input;
    quint64 m_serial;
    quint64 m_active;
    bool m_confirmed;
};

class InstallRequestClient : public QObject
{
    Q_OBJECT
public:
    explicit InstallRequestClient(const QString &endpoint, QObject *parent = nullptr);
    ~InstallRequestClient() override;

    // Thread-safe, non-blocking, and accepts at most one outstanding request.
    // True means queued for handoff, not installed.
    bool submit(const char *path, const char *name);

signals:
    void completed(const QString &outcome, const QString &message,
                   const QString &package, const QString &label, quint32 version);
    void sendRequested();

private:
    void sendQueued();
    void readAvailable();
    void disconnected();

    QLocalSocket *m_socket;
    QByteArray m_input;
    QMutex m_mutex;
    bool m_connected;
    bool m_busy;
    QByteArray m_pending;
};

#endif
