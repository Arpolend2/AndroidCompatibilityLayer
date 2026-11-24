#ifndef ANDROID_COMPATIBILITY_LAYER_EMBEDDED_GPU_TRANSPORT_H
#define ANDROID_COMPATIBILITY_LAYER_EMBEDDED_GPU_TRANSPORT_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <QObject>
#include <QStringList>
#include <QString>
#include <QSocketNotifier>
#include <QMutex>

#include <functional>

struct AuroraDmaBufFrame
{
    quint64 id = 0;
    quint64 channel = 0;
    quint64 generation = 0;
    bool probeOnly = false;
    int width = 0;
    int height = 0;
    int fourcc = 0;
    int planeCount = 0;
    quint64 modifier = 0;
    int fds[4] = {-1, -1, -1, -1};
    int strides[4] = {};
    int offsets[4] = {};

    AuroraDmaBufFrame() = default;
    AuroraDmaBufFrame(const AuroraDmaBufFrame &) = delete;
    AuroraDmaBufFrame &operator=(const AuroraDmaBufFrame &) = delete;
    AuroraDmaBufFrame(AuroraDmaBufFrame &&other) noexcept;
    AuroraDmaBufFrame &operator=(AuroraDmaBufFrame &&other) noexcept;
    ~AuroraDmaBufFrame();
    void close();
};

class EmbeddedGpuTransport : public QObject
{
    Q_OBJECT
public:
    explicit EmbeddedGpuTransport(QObject *parent = nullptr);
    ~EmbeddedGpuTransport() override;

    bool listen(const QString &path, qint64 expectedPeerPid, QString *error);
    bool connectTo(const QString &path, qint64 expectedPeerPid, QString *error);
    bool adoptInheritedSocket(int fd, bool receiveFrames, QString *error);
    void setExpectedPeerPid(qint64 pid);
    void close();
    void closePeer();
    bool connected() const;
    void setFrameHandler(const std::function<void(AuroraDmaBufFrame &&)> &handler);
    bool sendReply(quint64 frameId, bool accepted);
    bool sendFrame(const AuroraDmaBufFrame &frame, QString *error);
    bool presentTexture(GLuint texture, int width, int height, QString *error,
                        quint64 channel = 0, quint64 generation = 0);
    bool presentGbmTexture(GLuint texture, int width, int height, QString *error,
                           quint64 channel = 0, quint64 generation = 0);
    static bool canExportDmaBuf(QString *error);
    static QStringList currentGpuCapabilities();
    static QString probeGbmRenderTarget(int width, int height,
        const std::function<bool(const AuroraDmaBufFrame &, QString *)> &verifyParent = {},
        bool *available = nullptr);
    static bool sendFramePacket(int socket, const AuroraDmaBufFrame &frame);
    static bool receiveFramePacket(int socket, AuroraDmaBufFrame *frame);
    static bool sendReplyPacket(int socket, quint64 frameId, bool accepted);
    static bool receiveReplyPacket(int socket, quint64 frameId, bool *accepted);
    int duplicatePeer() const;

signals:
    void peerConnected();
    void peerDisconnected();
    void failed(const QString &reason);

private slots:
    void acceptPending();
    void readAvailable();

private:
    bool setPeer(int fd, qint64 expectedPeerPid, QString *error);
    void closePeerLocked();
    void fail(const QString &reason);

    mutable QMutex m_mutex;
    QMutex m_sendMutex;
    int m_listener = -1;
    int m_peer = -1;
    QString m_path;
    QSocketNotifier *m_listenerNotifier = nullptr;
    QSocketNotifier *m_peerNotifier = nullptr;
    qint64 m_expectedPeerPid = -1;
    bool m_server = false;
    std::function<void(AuroraDmaBufFrame &&)> m_frameHandler;
};

GLuint aurora_import_dma_buf_texture(const AuroraDmaBufFrame &frame, EGLDisplay display,
                                     QString *error);
bool aurora_copy_gpu_texture(GLuint source, GLuint target, int width, int height, QString *error);

#endif
