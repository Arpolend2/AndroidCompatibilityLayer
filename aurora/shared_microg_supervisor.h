#ifndef ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_SUPERVISOR_H
#define ANDROID_COMPATIBILITY_LAYER_SHARED_MICROG_SUPERVISOR_H

#include <QByteArray>
#include <QImage>
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
    bool sendTouch(quint64 channelId, int phase, int pointer, int x, int y);
    bool sendBack(quint64 channelId);
    bool sendText(quint64 channelId, const QByteArray &text);
    bool sendTextKey(quint64 channelId, int key);
    bool sendKeyboardClosed(quint64 channelId);
    bool sendKeyboardHeight(quint64 channelId, int height);
    bool sendUiResize(quint64 channelId, int width, int height, int density, int rotation);
    bool startRetained(QString *error);
    void releaseRetention();
    void closeChannel(quint64 channelId);
    void stop();

signals:
    void workerDied();
    void channelDisconnected(quint64 channelId);
    void authenticationUiChanged(quint64 channelId, bool active);
    void authenticationFrameChanged(quint64 channelId, const QImage &frame);
    void authenticationKeyboardRequested(quint64 channelId, int kind, int flags, int enter);
    void authenticationKeyboardHidden(quint64 channelId);

private:
    enum MessageType { Ready = 1, Open = 2, Close = 3, Stop = 4, Ack = 5, Nack = 6,
                       Disconnected = 7, UiStarted = 8, UiFinished = 9,
                       KeyboardShow = 10, KeyboardHide = 11, Touch = 12, Back = 13,
                       TextCommit = 14, TextKey = 15, KeyboardClosed = 16,
                       KeyboardHeight = 17, FrameDelivered = 18,
                       FrameSuppressed = 19, FrameSendFailed = 20, UiResize = 21 };
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
    bool receiveFrame(QImage *frame, quint64 *channelId, QString *error, bool *wouldBlock);
    void handleWorkerEvent(const Message &message);
    bool checkChild(bool reportDeath);
    void readUnsolicited();
    void readFrames();
    void handleDisconnected(quint64 channelId);
    void failWorker();
    void reapChild();
    void closeControlSocket();
    void closeFrameSocket();

    QString m_program;
    QStringList m_arguments;
    pid_t m_pid = -1;
    int m_controlFd = -1;
    int m_frameFd = -1;
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
    QSocketNotifier *m_frameNotifier = nullptr;
    QTimer m_reaper;
    quint64 m_frameSequence = 0;
    quint64 m_frameChannel = 0;
    QByteArray m_frameBuffer;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    int m_frameStride = 0;
    quint32 m_frameOffset = 0;
    quint64 m_authUiChannel = 0;
    quint64 m_lastFrameChannel = 0;
    QImage m_lastFrame;
};

#endif
