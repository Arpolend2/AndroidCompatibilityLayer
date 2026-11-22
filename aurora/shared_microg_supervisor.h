#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_SUPERVISOR_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_SUPERVISOR_H

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <sys/types.h>

class QSocketNotifier;

class SharedMicrogSupervisor : public QObject
{
    Q_OBJECT
public:
    explicit SharedMicrogSupervisor(QObject *parent = nullptr);
    ~SharedMicrogSupervisor() override;

    bool configure(const QString &program, const QStringList &arguments, QString *error);
    bool isConfigured() const { return !m_program.isEmpty(); }
    bool openChannel(const QString &package, int uid, int pid, quint64 channelId,
                     int serverFd, QString *error);
    bool startRetained(QString *error);
    void releaseRetention();
    void closeChannel(quint64 channelId);
    void stop();

signals:
    void workerDied();
    void channelDisconnected(quint64 channelId);

private:
    enum MessageType { Ready = 1, Open = 2, Close = 3, Stop = 4, Ack = 5, Nack = 6,
                       Disconnected = 7 };
    struct Message {
        MessageType type;
        quint64 channelId;
        quint32 uid;
        quint32 pid;
        quint32 packageLength;
    };

    bool start(QString *error);
    bool sendMessage(MessageType type, quint64 channelId, quint32 uid, quint32 pid,
                     const QByteArray &package, int fd, QString *error);
    bool waitForReply(MessageType expected, quint64 channelId, QString *error,
                      bool *rejected = nullptr, bool *disconnected = nullptr);
    bool receiveMessage(Message *message, QString *error, bool *wouldBlock = nullptr);
    bool checkChild(bool reportDeath);
    void readUnsolicited();
    void handleDisconnected(quint64 channelId);
    void failWorker();
    void reapChild();
    void closeControlSocket();

    QString m_program;
    QStringList m_arguments;
    pid_t m_pid = -1;
    int m_controlFd = -1;
    bool m_ready = false;
    bool m_stopping = false;
    bool m_retainWithoutClients = false;
    QSet<quint64> m_channels;
    QSet<quint64> m_usedChannelIds;
    QSet<quint64> m_closedChannelIds;
    QSet<quint64> m_openingChannels;
    QSet<quint64> m_closingChannels;
    QSet<quint64> m_disconnectedChannels;
    QSocketNotifier *m_socketNotifier = nullptr;
    QTimer m_reaper;
};

#endif
