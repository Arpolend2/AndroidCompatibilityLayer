#include "../embedded_gpu_transport.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString exportError;
    if (EmbeddedGpuTransport::canExportDmaBuf(&exportError) || exportError.isEmpty())
        return 15;

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
        return 1;
    const int source = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (source < 0) return 2;

    AuroraDmaBufFrame outgoing;
    outgoing.id = 42;
    outgoing.width = 32;
    outgoing.height = 16;
    outgoing.fourcc = 0x34325241;
    outgoing.planeCount = 1;
    outgoing.modifier = ~quint64(0);
    outgoing.fds[0] = source;
    outgoing.strides[0] = 128;
    outgoing.offsets[0] = 0;
    if (!EmbeddedGpuTransport::sendFramePacket(sockets[0], outgoing))
        return 3;

    AuroraDmaBufFrame incoming;
    if (!EmbeddedGpuTransport::receiveFramePacket(sockets[1], &incoming) ||
        incoming.id != outgoing.id || incoming.width != 32 || incoming.height != 16 ||
        incoming.fourcc != outgoing.fourcc || incoming.planeCount != 1 ||
        incoming.modifier != outgoing.modifier || incoming.strides[0] != 128 ||
        incoming.offsets[0] != 0 || incoming.fds[0] < 0 || incoming.fds[0] == source)
        return 4;

    const int receivedFd = incoming.fds[0];
    outgoing.close();
    if (fcntl(receivedFd, F_GETFD) < 0)
        return 5;
    bool accepted = false;
    if (!EmbeddedGpuTransport::sendReplyPacket(sockets[1], incoming.id, true) ||
        !EmbeddedGpuTransport::receiveReplyPacket(sockets[0], incoming.id, &accepted) || !accepted)
        return 6;

    AuroraDmaBufFrame invalid;
    invalid.id = 43;
    invalid.width = 32;
    invalid.height = 16;
    invalid.fourcc = outgoing.fourcc;
    invalid.planeCount = 1;
    invalid.fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    invalid.strides[0] = 0;
    if (invalid.fds[0] < 0 || EmbeddedGpuTransport::sendFramePacket(sockets[0], invalid))
        return 7;

    incoming.close();
    errno = 0;
    const bool closed = fcntl(receivedFd, F_GETFD) < 0 && errno == EBADF;
    const char malformedPacket = 0;
    if (send(sockets[0], &malformedPacket, sizeof(malformedPacket), 0) != sizeof(malformedPacket))
        return 16;
    errno = 0;
    if (EmbeddedGpuTransport::receiveFramePacket(sockets[1], &incoming) || errno != EPROTO)
        return 17;
    invalid.close();
    close(sockets[0]);
    close(sockets[1]);
    if (!closed) return 8;

    QTemporaryDir directory;
    if (!directory.isValid()) return 9;
    EmbeddedGpuTransport server;
    EmbeddedGpuTransport client;
    QString error;
    const QString endpoint = directory.path() + QStringLiteral("/gpu");
    if (!server.listen(endpoint, getpid(), &error) || !client.connectTo(endpoint, getpid(), &error))
        return 10;

    QEventLoop connectedLoop;
    QObject::connect(&server, &EmbeddedGpuTransport::peerConnected, &connectedLoop, &QEventLoop::quit);
    QTimer::singleShot(1000, &connectedLoop, &QEventLoop::quit);
    if (!server.connected()) connectedLoop.exec();
    if (!server.connected()) return 11;

    AuroraDmaBufFrame channelFrame;
    channelFrame.id = 99;
    channelFrame.width = 16;
    channelFrame.height = 8;
    channelFrame.fourcc = 0x34325241;
    channelFrame.planeCount = 1;
    channelFrame.modifier = ~quint64(0);
    channelFrame.fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    channelFrame.strides[0] = 64;
    if (channelFrame.fds[0] < 0) return 12;

    bool frameReceived = false;
    QEventLoop frameLoop;
    server.setFrameHandler([&](AuroraDmaBufFrame &&frame) {
        frameReceived = frame.id == 99 && frame.width == 16 && frame.height == 8 &&
                        frame.planeCount == 1 && frame.fds[0] >= 0;
        const bool replied = server.sendReply(frame.id, frameReceived);
        frame.close();
        if (!replied) frameReceived = false;
        frameLoop.quit();
    });
    std::atomic<bool> sent(false);
    std::thread sender([&]() { sent.store(client.sendFrame(channelFrame, &error)); });
    QTimer::singleShot(1500, &frameLoop, &QEventLoop::quit);
    frameLoop.exec();
    sender.join();
    if (!frameReceived || !sent.load()) return 13;

    bool reportedFailure = false;
    bool disconnected = false;
    QObject::connect(&server, &EmbeddedGpuTransport::failed, [&](const QString &) {
        reportedFailure = true;
    });
    QObject::connect(&server, &EmbeddedGpuTransport::peerDisconnected, [&]() {
        disconnected = true;
    });
    QEventLoop disconnectLoop;
    QTimer::singleShot(1000, &disconnectLoop, &QEventLoop::quit);
    client.close();
    disconnectLoop.exec();
    return !server.connected() && disconnected && !reportedFailure ? 0 : 14;
}
