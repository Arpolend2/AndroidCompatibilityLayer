#include "embedded_preview.h"
#include <QElapsedTimer>

#include "picker_host.h"
#include "preview_keyboard.h"
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
#include "preview_gpu.h"
#endif

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QTimer>
#include <QtEndian>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
const quint32 maxFrameBytes = 32 * 1024 * 1024;
const quint32 maxControlBytes = 64 * 1024;

QByteArray packet(const QByteArray &body)
{
    QByteArray result(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), reinterpret_cast<uchar *>(result.data()));
    result += body;
    return result;
}

bool peerIs(QLocalSocket *socket, qint64 pid)
{
    struct ucred credentials = {};
    socklen_t size = sizeof(credentials);
    return pid > 0 && getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                                  &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.pid == pid;
}

bool number(const QJsonObject &o, const char *name, double min, double max, double *out)
{
    const QJsonValue value = o.value(QLatin1String(name));
    if (!value.isDouble() || value.toDouble() < min || value.toDouble() > max) return false;
    *out = value.toDouble();
    return true;
}
}

EmbeddedChannel::EmbeddedChannel(QObject *parent) : QObject(parent) {}

EmbeddedChannel::~EmbeddedChannel() { closeShared(); }

void EmbeddedChannel::attach(QLocalSocket *socket)
{
    m_socket = socket;
    m_failed = false;
    m_input.clear();
    socket->setReadBufferSize(qint64(maxFrameBytes) + 4);
    connect(socket, &QLocalSocket::readyRead, this, &EmbeddedChannel::read);
    connect(socket, &QLocalSocket::disconnected, this, [this]() { fail(tr("Preview channel disconnected.")); });
    if (socket->bytesAvailable()) read();
}

void EmbeddedChannel::reset()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket.clear();
    }
    m_input.clear();
    m_failed = false;
    closeShared();
}

void EmbeddedChannel::useSharedFrames(const QString &path)
{
    closeShared();
    m_sharedPath = path;
}

bool EmbeddedChannel::ensureShared(qint64 bytes, bool writable)
{
    if (m_sharedPath.isEmpty() || bytes <= 0 || bytes > qint64(maxFrameBytes)) return false;
    if (m_sharedFd >= 0 && m_sharedBytes == bytes && m_sharedWritable == writable) return true;
    closeShared();
    const QByteArray path = QFile::encodeName(m_sharedPath);
    const int fd = writable ? ::open(path.constData(), O_RDWR | O_CREAT, 0600)
                            : ::open(path.constData(), O_RDONLY);
    if (fd < 0) return false;
    if (writable && ::ftruncate(fd, off_t(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void *mapped = ::mmap(nullptr, size_t(bytes), writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                          MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    m_sharedFd = fd;
    m_sharedMap = static_cast<uchar *>(mapped);
    m_sharedBytes = bytes;
    m_sharedWritable = writable;
    if (writable) ++m_sharedGeneration;
    return true;
}

void EmbeddedChannel::closeShared()
{
    if (m_sharedMap) ::munmap(m_sharedMap, size_t(m_sharedBytes));
    m_sharedMap = nullptr;
    if (m_sharedFd >= 0) ::close(m_sharedFd);
    m_sharedFd = -1;
    m_sharedBytes = 0;
}

bool EmbeddedChannel::connected() const
{
    return m_socket && !m_failed && m_socket->state() == QLocalSocket::ConnectedState;
}

bool EmbeddedChannel::send(const QJsonObject &message)
{
    const QByteArray body = QByteArray("J") + QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (!connected() || body.size() > int(maxControlBytes) ||
        m_socket->bytesToWrite() > qint64(maxFrameBytes) ||
        m_socket->write(packet(body)) != body.size() + 4) {
        fail(tr("Cannot send preview control message."));
        return false;
    }
    return true;
}

bool EmbeddedChannel::sendFrame(const QImage &frame)
{
    const qint64 bytes = qint64(frame.width()) * frame.height() * 4;
    if (!connected() || frame.isNull() || frame.format() != QImage::Format_RGB32 ||
        frame.width() > 4096 || frame.height() > 4096 || bytes > maxFrameBytes - 9 ||
        frame.bytesPerLine() != frame.width() * 4) {
        fail(tr("Invalid preview frame."));
        return false;
    }
    if (ensureShared(bytes, true)) {
        std::memcpy(m_sharedMap, frame.constBits(), size_t(bytes));
        QByteArray body(21, '\0');
        body[0] = 'S';
        qToBigEndian<quint32>(quint32(frame.width()), reinterpret_cast<uchar *>(body.data() + 1));
        qToBigEndian<quint32>(quint32(frame.height()), reinterpret_cast<uchar *>(body.data() + 5));
        qToBigEndian<quint64>(quint64(bytes), reinterpret_cast<uchar *>(body.data() + 9));
        qToBigEndian<quint32>(m_sharedGeneration, reinterpret_cast<uchar *>(body.data() + 17));
        const QByteArray output = packet(body);
        if (m_socket->write(output) != output.size()) {
            fail(tr("Cannot send preview frame."));
            return false;
        }
        return true;
    }
    QByteArray body(9, '\0');
    body[0] = 'F';
    qToBigEndian<quint32>(quint32(frame.width()), reinterpret_cast<uchar *>(body.data() + 1));
    qToBigEndian<quint32>(quint32(frame.height()), reinterpret_cast<uchar *>(body.data() + 5));
    body.append(reinterpret_cast<const char *>(frame.constBits()), int(bytes));
    const QByteArray output = packet(body);
    if (m_socket->write(output) != output.size()) {
        fail(tr("Cannot send preview frame."));
        return false;
    }
    return true;
}

void EmbeddedChannel::read()
{
    while (connected() && m_socket->bytesAvailable()) {
        const qint64 room = qint64(maxFrameBytes) + 4 - m_input.size();
        if (room <= 0) { fail(tr("Preview message is too large.")); return; }
        m_input += m_socket->read(qMin(room, m_socket->bytesAvailable()));
        while (m_input.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(m_input.constData()));
            if (!size || size > maxFrameBytes) { fail(tr("Invalid preview message size.")); return; }
            if (m_input.size() < qint64(size) + 4) break;
            const QByteArray body = m_input.mid(4, int(size));
            m_input.remove(0, int(size) + 4);
            if (body[0] == 'J') {
                QJsonParseError error;
                const QJsonDocument doc = QJsonDocument::fromJson(body.mid(1), &error);
                if (size > maxControlBytes || error.error != QJsonParseError::NoError || !doc.isObject()) {
                    fail(tr("Invalid preview control message.")); return;
                }
                emit message(doc.object());
            } else if (body[0] == 'S' && size == 21) {
                const uchar *data = reinterpret_cast<const uchar *>(body.constData());
                const quint32 w = qFromBigEndian<quint32>(data + 1);
                const quint32 h = qFromBigEndian<quint32>(data + 5);
                const quint64 bytes = qFromBigEndian<quint64>(data + 9);
                if (!w || !h || w > 4096 || h > 4096 || bytes != quint64(w) * h * 4 ||
                    bytes > maxFrameBytes || !ensureShared(qint64(bytes), false)) {
                    fail(tr("Invalid shared preview frame.")); return;
                }
                if (!m_sharedLogged) {
                    m_sharedLogged = true;
                    qWarning().noquote() << "Embedded preview frames: shared memory, no pixels over the socket:"
                                         << m_sharedPath;
                }
                const QImage image(m_sharedMap, int(w), int(h), int(w * 4), QImage::Format_RGB32);
                emit frame(image.copy());
            } else if (body[0] == 'F' && size >= 9) {
                const uchar *data = reinterpret_cast<const uchar *>(body.constData());
                const quint32 w = qFromBigEndian<quint32>(data + 1);
                const quint32 h = qFromBigEndian<quint32>(data + 5);
                if (!w || !h || w > 4096 || h > 4096 || quint64(w) * h * 4 != size - 9) {
                    fail(tr("Invalid preview frame size.")); return;
                }
                const QImage image(data + 9, int(w), int(h), int(w * 4), QImage::Format_RGB32);
                emit frame(image.copy());
            } else {
                fail(tr("Invalid preview message.")); return;
            }
        }
    }
}

void EmbeddedChannel::fail(const QString &reason)
{
    if (m_failed) return;
    m_failed = true;
    if (m_socket) m_socket->abort();
    emit failed(reason);
}

EmbeddedPreview::EmbeddedPreview(QObject *parent)
    : PreviewPlayer(QString(), QString(), QString(), parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, &EmbeddedPreview::acceptPending);
    m_connectionTimer.setSingleShot(true);
    connect(&m_connectionTimer, &QTimer::timeout, this, [this]() {
        if (!m_process || m_channel.connected()) return;
        m_status = tr("Android preview did not connect.");
        emit remoteStatusChanged();
        emit channelError(m_status);
        m_process->kill();
    });
    connect(&m_channel, &EmbeddedChannel::message, this, &EmbeddedPreview::receive);
    connect(&m_channel, &EmbeddedChannel::frame, this, [this](const QImage &image) {
#ifdef MG_EXPERIMENTAL_GPU_COMPOSITION
        gpuState()->clear();
#endif
        m_frame = image;
        emit frameChanged();
        command(QStringLiteral("frameAck"));
    });
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    m_gpuTransport.setFrameHandler([this](AuroraDmaBufFrame &&frame) {
        receiveGpuFrame(std::move(frame));
    });
    connect(&m_gpuTransport, &EmbeddedGpuTransport::failed, this, [this](const QString &reason) {
        gpuState()->clear();
        emit frameChanged();
        if (!m_stopping && !m_restarting) {
            qWarning().noquote() << "Embedded GPU transport unavailable; CPU fallback remains active:" << reason;
            reportGraphicsError(tr("Embedded GPU transport; CPU fallback active"), reason);
        }
    });
    connect(&m_gpuTransport, &EmbeddedGpuTransport::peerDisconnected, this, [this]() {
        gpuState()->clear();
        emit frameChanged();
    });
#endif
    connect(&m_channel, &EmbeddedChannel::failed, this, [this](const QString &error) {
        if (!m_stopping && !m_restarting) {
            m_status = error;
            emit remoteStatusChanged();
            emit channelError(error);
        }
    });
}

bool EmbeddedPreview::listen(QProcess *process, QString *error)
{
    if (error) error->clear();
    if (!process || !m_directory.isValid() || m_server.isListening() || m_channel.connected()) {
        if (error) *error = tr("Cannot initialize the embedded preview channel.");
        return false;
    }
    m_process = process;
    m_channel.reset();
    m_stopping = false;
    m_restarting = false;
    if (m_keyboard) m_keyboard->start();
    m_frame = QImage();
    /* Frames from the worker do not cross the socket: both processes map <dir>/frame, and a
       message carries only the size (EmbeddedChannel::useSharedFrames). */
    const QString sharedFramePath = m_directory.path() + QStringLiteral("/frame");
    QFile::remove(sharedFramePath);
    m_channel.useSharedFrames(sharedFramePath);
    m_status = tr("Starting Android app...");
    emit remoteStatusChanged();
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    m_gpuTransport.close();
    m_gpuEndpoint = m_directory.path() + QStringLiteral("/gpu");
    QString gpuError;
    if (!m_gpuTransport.listen(m_gpuEndpoint, -1, &gpuError)) {
        qWarning().noquote() << "Embedded GPU transport unavailable; CPU fallback remains active:" << gpuError;
        m_gpuEndpoint.clear();
    }
#endif
    if (!m_server.listen(m_directory.path() + QStringLiteral("/channel"))) {
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        m_gpuTransport.close();
        m_gpuEndpoint.clear();
#endif
        if (error) *error = m_server.errorString();
        return false;
    }
    connect(process, &QProcess::started, this, &EmbeddedPreview::acceptPending, Qt::UniqueConnection);
    connect(process, &QProcess::started, this, [this, process]() {
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        if (m_process == process)
            m_gpuTransport.setExpectedPeerPid(process->processId());
#endif
        if (m_process == process && !m_channel.connected()) m_connectionTimer.start(15000);
    });
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process](int, QProcess::ExitStatus) {
        if (m_process != process) return;
        m_server.close();
        m_connectionTimer.stop();
        m_channel.reset();
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        m_gpuTransport.close();
        m_gpuEndpoint.clear();
        gpuState()->clear();
#endif
        m_process.clear();
        if (m_keyboard) m_keyboard->stop();
        if (!m_stopping) {
            m_status = tr("Android app ended.");
            emit remoteStatusChanged();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process != process || error != QProcess::FailedToStart) return;
        m_server.close();
        m_connectionTimer.stop();
        m_channel.reset();
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        m_gpuTransport.close();
        m_gpuEndpoint.clear();
        gpuState()->clear();
#endif
        m_process.clear();
        m_status = tr("Could not start Android app: %1").arg(process->errorString());
        emit remoteStatusChanged();
    });
    return true;
}

#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
void EmbeddedPreview::receiveGpuFrame(AuroraDmaBufFrame &&frame)
{
    const quint64 id = frame.id;
    QImage placeholder(frame.width, frame.height, QImage::Format_RGB32);
    if (placeholder.isNull()) {
        m_gpuTransport.sendReply(id, false);
        return;
    }
    placeholder.fill(Qt::black);
    QPointer<EmbeddedPreview> self(this);
    if (!gpuState()->submitDmaBuf(std::move(frame), this, [self](quint64 frameId, bool accepted) {
            if (self) self->m_gpuTransport.sendReply(frameId, accepted);
        })) {
        m_gpuTransport.sendReply(id, false);
        const QString reason = gpuState()->lastFailure();
        qWarning().noquote() << "Embedded GPU frame rejected; using CPU fallback:" << reason;
        reportGraphicsError(tr("Qt native-buffer import; CPU fallback active"), reason);
        return;
    }
    m_frame = placeholder;
    emit remoteStatusChanged();
    emit frameChanged();
}
#endif

QString EmbeddedPreview::endpoint() const { return m_server.fullServerName(); }

void EmbeddedPreview::acceptPending()
{
    if (!m_process || m_process->processId() <= 0) return;
    while (m_server.hasPendingConnections()) {
        QLocalSocket *socket = m_server.nextPendingConnection();
        if (m_channel.connected() || !peerIs(socket, m_process->processId())) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        m_channel.attach(socket);
        m_connectionTimer.stop();
        m_server.close();
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        if (!m_gpuEndpoint.isEmpty())
            command(QStringLiteral("gpuConnect"), QJsonObject{{QStringLiteral("endpoint"), m_gpuEndpoint}});
#endif
        if (m_stopping) {
            command(QStringLiteral("stop"));
            continue;
        }
        setRemoteTheme(m_dark, m_accent);
        if (m_width > 0 && m_height > 0) setViewport(m_width, m_height, m_ratio);
        command(QStringLiteral("uiReady"), QJsonObject{{QStringLiteral("ready"), m_ready}});
    }
}

void EmbeddedPreview::setRemoteTheme(bool dark, const QColor &accent)
{
    m_dark = dark;
    m_accent = accent;
    command(QStringLiteral("theme"), QJsonObject{{QStringLiteral("dark"), dark},
            {QStringLiteral("accent"), accent.name(QColor::HexArgb)}});
}

void EmbeddedPreview::setRemoteKeyboard(PreviewKeyboard *keyboard)
{
    m_keyboard = keyboard;
    PreviewPlayer::setKeyboard(keyboard);
    keyboard->setRemote(true);
    keyboard->start();
    connect(keyboard, &PreviewKeyboard::textCommitted, this, [this](const QString &text) {
        command(QStringLiteral("text"), QJsonObject{{QStringLiteral("text"), text}});
    });
    connect(keyboard, &PreviewKeyboard::textKey, this, [this](int key) {
        command(QStringLiteral("textKey"), QJsonObject{{QStringLiteral("key"), key}});
    });
    connect(keyboard, &PreviewKeyboard::keyboardClosed, this, [this]() { command(QStringLiteral("keyboardClosed")); });
    connect(keyboard, &PreviewKeyboard::keyboardHeight, this, [this](int height) {
        command(QStringLiteral("keyboardHeight"), QJsonObject{{QStringLiteral("height"), height}});
    });
}

void EmbeddedPreview::command(const QString &type, const QJsonObject &values)
{
    if (!m_channel.connected()) return;
    QJsonObject message(values);
    message.insert(QStringLiteral("type"), type);
    m_channel.send(message);
}

bool EmbeddedPreview::touch(int phase, float x, float y)
{
    if (!m_channel.connected() || m_stopping) return false;
    command(QStringLiteral("touch"), QJsonObject{{QStringLiteral("phase"), phase},
            {QStringLiteral("x"), double(x)}, {QStringLiteral("y"), double(y)}});
    return true;
}

void EmbeddedPreview::back()
{
    if (m_keyboard && m_keyboard->back()) return;
    command(QStringLiteral("back"));
}

void EmbeddedPreview::setViewport(qreal width, qreal height, qreal ratio)
{
    PreviewPlayer::setViewport(width, height, ratio);
    m_width = width; m_height = height; m_ratio = ratio;
    command(QStringLiteral("viewport"), QJsonObject{{QStringLiteral("width"), width},
            {QStringLiteral("height"), height}, {QStringLiteral("ratio"), ratio}});
}

void EmbeddedPreview::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    if (m_keyboard) m_keyboard->stop();
    command(QStringLiteral("stop"));
    QPointer<QProcess> process = m_process;
    QTimer::singleShot(7000, this, [this, process]() {
        if (process && process == m_process && process->state() != QProcess::NotRunning) {
            qWarning("Embedded Android preview did not stop in time.");
            process->kill();
        }
    });
}

void EmbeddedPreview::expectRestart()
{
    m_restarting = true;
    m_status = tr("Restarting Android app...");
    emit remoteStatusChanged();
}

void EmbeddedPreview::close()
{
    stop();
}

void EmbeddedPreview::closeToManager()
{
    if (!closeOnBackGesture() || m_stopping) return;
    qInfo("Embedded preview: explicit return to manager.");
    emit returnToManagerRequested();
    stop();
}

void EmbeddedPreview::setUiReady(bool ready)
{
    m_ready = ready;
    command(QStringLiteral("uiReady"), QJsonObject{{QStringLiteral("ready"), ready}});
}

void EmbeddedPreview::selected(const QString &token, const QString &path)
{
    command(QStringLiteral("selected"), QJsonObject{{QStringLiteral("token"), token}, {QStringLiteral("path"), path}});
}
void EmbeddedPreview::canceled(const QString &token)
{
    command(QStringLiteral("canceled"), QJsonObject{{QStringLiteral("token"), token}});
}
void EmbeddedPreview::showFailed(const QString &token)
{
    command(QStringLiteral("pickerFailed"), QJsonObject{{QStringLiteral("token"), token}});
}

void EmbeddedPreview::receive(const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("shutdownComplete")) {
        if (m_stopping) emit shutdownComplete();
    } else if (type == QLatin1String("graphicsError")) {
        reportGraphicsError(tr("Android worker graphics; CPU fallback active"),
                            message.value(QStringLiteral("message")).toString());
    } else if (type == QLatin1String("status")) {
        m_status = message.value(QStringLiteral("status")).toString();
        emit remoteStatusChanged();
    } else if (type == QLatin1String("keyboardShow")) {
        double kind, flags, enter;
        if (m_keyboard && number(message, "kind", 0, 100, &kind) &&
            number(message, "flags", 0, 65535, &flags) && number(message, "enter", 0, 100, &enter))
            m_keyboard->showKeyboard(int(kind), int(flags), int(enter));
    } else if (type == QLatin1String("keyboardHide")) {
        if (m_keyboard) m_keyboard->hideKeyboard();
    } else if (type == QLatin1String("picker")) {
        const QString token = message.value(QStringLiteral("token")).toString();
        const QJsonArray array = message.value(QStringLiteral("filters")).toArray();
        QStringList filters;
        for (const QJsonValue &value : array) if (value.isString()) filters << value.toString();
        if (!token.isEmpty() && !filters.isEmpty()) emit showPicker(token, filters);
        else {
            qWarning("Invalid embedded preview picker request.");
            if (!token.isEmpty()) showFailed(token);
        }
    } else {
        qWarning("Unknown embedded preview message.");
    }
}

EmbeddedPreviewChild::EmbeddedPreviewChild(PreviewPlayer *player, AuroraPickerHost *picker, QObject *parent)
    : QObject(parent), m_player(player), m_picker(picker)
{
    connect(&m_channel, &EmbeddedChannel::message, this, &EmbeddedPreviewChild::receive);
    connect(&m_channel, &EmbeddedChannel::failed, this, &EmbeddedPreviewChild::failed);
    connect(player, &PreviewPlayer::statusChanged, this, &EmbeddedPreviewChild::sendStatus);
    connect(player, &PreviewPlayer::frameChanged, this, [this]() {
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        if (!m_gpuPresenting.loadAcquire()) {
#endif
            m_framePending = true;
            sendFrame();
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
        }
#endif
        sendStatus();
    });
    connect(picker, &AuroraPickerHost::showPicker, this, [this](const QString &token, const QStringList &filters) {
        command(QStringLiteral("picker"), QJsonObject{{QStringLiteral("token"), token},
                {QStringLiteral("filters"), QJsonArray::fromStringList(filters)}});
    });
}

bool EmbeddedPreviewChild::connectTo(const QString &endpoint, QString *error)
{
    if (error) error->clear();
    m_socket.connectToServer(endpoint);
    if (!m_socket.waitForConnected(5000) || !peerIs(&m_socket, getppid())) {
        if (error) *error = tr("Cannot authenticate the embedded preview manager.");
        m_socket.abort();
        return false;
    }
    m_channel.useSharedFrames(QFileInfo(endpoint).absolutePath() + QStringLiteral("/frame"));
    m_channel.attach(&m_socket);
    return true;
}

void EmbeddedPreviewChild::notifyShutdownComplete()
{
    if (!m_stopping || !m_channel.connected()) return;
    if (!m_channel.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("shutdownComplete")}})) return;
    // The worker's event loop has exited; drain the notification before teardown.
    QElapsedTimer elapsed;
    elapsed.start();
    m_socket.flush();
    while (m_socket.bytesToWrite() > 0) {
        const qint64 left = 100 - elapsed.elapsed();
        if (left <= 0 || !m_socket.waitForBytesWritten(int(left))) {
            qWarning("Cannot flush embedded preview shutdown confirmation.");
            break;
        }
    }
}

void EmbeddedPreviewChild::command(const QString &type, const QJsonObject &values)
{
    QJsonObject message(values);
    message.insert(QStringLiteral("type"), type);
    m_channel.send(message);
}

#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
bool MEGLER_CB EmbeddedPreviewChild::presentGpuCanvas(void *user, uint32_t texture, int width, int height)
{
    EmbeddedPreviewChild *self = static_cast<EmbeddedPreviewChild *>(user);
    if (!self || self->m_gpuFailed.loadAcquire()) return false;
    QString error;
    const bool accepted = self->m_gpuTransport.presentTexture(texture, width, height, &error);
    self->m_gpuPresenting.storeRelease(accepted ? 1 : 0);
    if (accepted) {
        if (self->m_gpuActiveLogged.testAndSetOrdered(0, 1))
            qInfo("Embedded GPU native/DMA-BUF frame imported and acknowledged; CPU readback is disabled");
    } else {
        self->m_gpuFailed.storeRelease(1);
        self->m_gpuTransport.closePeer();
        const QString reason = error.isEmpty() ? QStringLiteral("GPU channel is unavailable") : error;
        qWarning().noquote() << "Embedded GPU presentation unavailable; using CPU fallback:" << reason;
        self->m_player->reportGraphicsError(QStringLiteral("Worker gralloc/DMA-BUF presentation; CPU fallback active"), reason);
        self->command(QStringLiteral("graphicsError"), QJsonObject{{QStringLiteral("message"), reason}});
    }
    return accepted;
}
#endif

void EmbeddedPreviewChild::sendFrame()
{
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    if (m_gpuPresenting.loadAcquire()) return;
#endif
    if (!m_framePending || m_frameOutstanding || !m_channel.connected()) return;
    const QImage frame = m_player->frame();
    if (frame.isNull()) return;
    m_framePending = false;
    m_frameOutstanding = true;
    m_channel.sendFrame(frame);
}

void EmbeddedPreviewChild::sendStatus()
{
    const QString status = m_player->showStatus() ? m_player->status() : QString();
    if (m_statusSent && status == m_lastStatus) return;
    m_statusSent = true;
    m_lastStatus = status;
    command(QStringLiteral("status"), QJsonObject{{QStringLiteral("status"), status}});
}

void EmbeddedPreviewChild::maybeStart()
{
    if (!m_uiReady || !m_haveViewport || m_startScheduled || m_stopping) return;
    m_startScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        if (!m_uiReady) m_startScheduled = false;
        else if (!m_stopping) m_player->start();
    });
}

megler_keyboard_host EmbeddedPreviewChild::keyboardHost()
{
    megler_keyboard_host host = {};
    host.user = this;
    host.show = requestShow;
    host.hide = requestHide;
    return host;
}

void MEGLER_CB EmbeddedPreviewChild::requestShow(void *user, int kind, int flags, int enter)
{
    QMetaObject::invokeMethod(static_cast<EmbeddedPreviewChild *>(user), "showKeyboard", Qt::QueuedConnection,
                              Q_ARG(int, kind), Q_ARG(int, flags), Q_ARG(int, enter));
}
void MEGLER_CB EmbeddedPreviewChild::requestHide(void *user)
{
    QMetaObject::invokeMethod(static_cast<EmbeddedPreviewChild *>(user), "hideKeyboard", Qt::QueuedConnection);
}
void EmbeddedPreviewChild::showKeyboard(int kind, int flags, int enter)
{
    command(QStringLiteral("keyboardShow"), QJsonObject{{QStringLiteral("kind"), kind},
            {QStringLiteral("flags"), flags}, {QStringLiteral("enter"), enter}});
}
void EmbeddedPreviewChild::hideKeyboard() { command(QStringLiteral("keyboardHide")); }

void EmbeddedPreviewChild::receive(const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    double a, b, c;
#if defined(MG_EXPERIMENTAL_GPU_COMPOSITION) && defined(MG_EXPERIMENTAL_GPU_CANVAS)
    if (type == QLatin1String("gpuConnect")) {
        const QString endpoint = message.value(QStringLiteral("endpoint")).toString();
        QString error;
        if (endpoint.isEmpty() || !m_gpuTransport.connectTo(endpoint, getppid(), &error)) {
            qWarning().noquote() << "Embedded GPU channel unavailable; using CPU presentation:"
                                 << (error.isEmpty() ? QStringLiteral("missing endpoint") : error);
        } else {
            m_player->setRemoteGpuCanvasPresentation(presentGpuCanvas, this);
            qInfo("Embedded worker authenticated a GPU channel to its parent");
        }
        maybeStart();
    } else
#endif
    if (type == QLatin1String("frameAck")) {
        m_frameOutstanding = false;
        sendFrame();
    } else if (type == QLatin1String("theme")) {
        const QColor accent(message.value(QStringLiteral("accent")).toString());
        if (accent.isValid()) m_player->setSystemTheme(message.value(QStringLiteral("dark")).toBool(), accent);
    } else if (type == QLatin1String("viewport") &&
               number(message, "width", 1, 8192, &a) && number(message, "height", 1, 8192, &b) &&
               number(message, "ratio", 0.1, 8, &c)) {
        m_player->setViewport(a, b, c);
        m_haveViewport = true;
        maybeStart();
    } else if (type == QLatin1String("touch") &&
               number(message, "phase", 0, 3, &a) && number(message, "x", 0, 8192, &b) &&
               number(message, "y", 0, 8192, &c)) {
        m_player->touch(int(a), float(b), float(c));
    } else if (type == QLatin1String("back")) {
        m_player->back();
    } else if (type == QLatin1String("stop")) {
        m_stopping = true;
        m_player->close();
    } else if (type == QLatin1String("uiReady")) {
        m_uiReady = message.value(QStringLiteral("ready")).toBool();
        m_picker->setUiReady(m_uiReady);
        maybeStart();
    } else if (type == QLatin1String("selected")) {
        m_picker->selected(message.value(QStringLiteral("token")).toString(), message.value(QStringLiteral("path")).toString());
    } else if (type == QLatin1String("canceled")) {
        m_picker->canceled(message.value(QStringLiteral("token")).toString());
    } else if (type == QLatin1String("pickerFailed")) {
        m_picker->showFailed(message.value(QStringLiteral("token")).toString());
    } else if (type == QLatin1String("text")) {
        const QByteArray text = message.value(QStringLiteral("text")).toString().toUtf8();
        megler_text_commit(text.constData());
    } else if (type == QLatin1String("textKey") && number(message, "key", 0, 100, &a)) {
        megler_text_key(int(a));
    } else if (type == QLatin1String("keyboardClosed")) {
        megler_keyboard_closed();
    } else if (type == QLatin1String("keyboardHeight") && number(message, "height", 0, 8192, &a)) {
        megler_keyboard_height(int(a));
    } else {
        qWarning("Invalid embedded preview command.");
    }
}
