#pragma once

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QTemporaryDir>

class QLocalServer;
class QLocalSocket;
class QProcess;
class QTimer;
class QWindow;

class PreviewActivationServer : public QObject
{
    Q_OBJECT
public:
    explicit PreviewActivationServer(QProcess *process, QObject *parent = nullptr);
    ~PreviewActivationServer() override;
    bool listen(QString *error);
    QString endpoint() const;
    bool request(int timeoutMs, QString *error);

signals:
    void completed(bool activated, const QString &error);

private:
    void acceptPending();
    void readAvailable();
    void closeConnection(const QString &error);
    void finish(bool activated, const QString &error);
    void sendPending();

    QProcess *m_process;
    QTemporaryDir m_directory;
    QLocalServer *m_server;
    QLocalSocket *m_socket = nullptr;
    QTimer *m_timer;
    QByteArray m_input;
    quint64 m_serial = 0;
    quint64 m_pending = 0;
    qint64 m_deadline = 0;
    bool m_sent = false;
    bool m_disconnected = false;
};

class PreviewActivationClient : public QObject
{
    Q_OBJECT
public:
    PreviewActivationClient(const QString &endpoint, QWindow *window, QObject *parent = nullptr);
    bool start(QString *error);

signals:
    void channelError(const QString &error);

private:
    void readAvailable();
    void activeChanged();
    void respond(quint64 id, bool activated);
    void fail(const QString &error);

    QLocalSocket *m_socket;
    QString m_endpoint;
    QPointer<QWindow> m_window;
    QByteArray m_input;
    quint64 m_pending = 0;
    qint64 m_deadline = 0;
    bool m_failed = false;
};
