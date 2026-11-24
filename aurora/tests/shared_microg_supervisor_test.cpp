#include "../shared_microg_supervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
enum { Ready = 1, Open = 2, Close = 3, Stop = 4, Ack = 5, Nack = 6, Disconnected = 7 };
enum { HeaderSize = 28, FrameHeaderSize = 48, FrameChunk = 60 * 1024 };

bool sawFrameDelivered = false;
bool sawFrameSuppressed = false;

void captureDiagnostics(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.startsWith(QLatin1String("Shared microG UI frame delivered:")))
        sawFrameDelivered = true;
    if (message.startsWith(QLatin1String("Shared microG UI frame suppressed:")))
        sawFrameSuppressed = true;
}

void write32(char *buffer, quint32 value)
{
    buffer[0] = static_cast<char>(value >> 24);
    buffer[1] = static_cast<char>(value >> 16);
    buffer[2] = static_cast<char>(value >> 8);
    buffer[3] = static_cast<char>(value);
}

void write64(char *buffer, quint64 value)
{
    for (int i = 7; i >= 0; --i) {
        buffer[i] = static_cast<char>(value);
        value >>= 8;
    }
}

quint32 read32(const char *buffer)
{
    return (static_cast<quint32>(static_cast<unsigned char>(buffer[0])) << 24) |
           (static_cast<quint32>(static_cast<unsigned char>(buffer[1])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(buffer[2])) << 8) |
           static_cast<quint32>(static_cast<unsigned char>(buffer[3]));
}

quint64 read64(const char *buffer)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(buffer[i]);
    return value;
}

bool sendReply(int fd, quint32 type, quint64 channelId)
{
    char packet[HeaderSize] = { 'M', 'G', 'R', '1' };
    write32(packet + 4, type);
    write64(packet + 8, channelId);
    return ::send(fd, packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet);
}

bool sendFrameDiagnostic(int fd, quint32 type, quint64 channelId)
{
    char packet[HeaderSize] = { 'M', 'G', 'R', '1' };
    write32(packet + 4, type);
    write64(packet + 8, channelId);
    write32(packet + 16, 800);
    write32(packet + 20, 480);
    write32(packet + 24, 0);
    return ::send(fd, packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet);
}

bool sendKeyboardShow(int fd, quint64 channelId, quint32 enter)
{
    char packet[HeaderSize] = { 'M', 'G', 'R', '1' };
    write32(packet + 4, 10);
    write64(packet + 8, channelId);
    write32(packet + 16, 3);
    write32(packet + 20, 128);
    write32(packet + 24, enter);
    return ::send(fd, packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet);
}

bool sendFrame(int fd, quint64 channelId, quint64 sequence, bool nonzero)
{
    const quint32 width = 800, height = 480, stride = width * 4;
    const quint32 total = stride * height;
    char packet[FrameHeaderSize + FrameChunk];
    for (quint32 offset = 0; offset < total;) {
        quint32 chunk = total - offset;
        if (chunk > FrameChunk) chunk = FrameChunk;
        std::memcpy(packet, "MGUI", 4);
        write32(packet + 4, 1);
        write64(packet + 8, channelId);
        write64(packet + 16, sequence);
        write32(packet + 24, width);
        write32(packet + 28, height);
        write32(packet + 32, stride);
        write32(packet + 36, total);
        write32(packet + 40, offset);
        write32(packet + 44, chunk);
        unsigned char *pixels = reinterpret_cast<unsigned char *>(packet + FrameHeaderSize);
        for (quint32 i = 0; i < chunk; ++i)
            pixels[i] = nonzero ? static_cast<unsigned char>((offset + i) * 37 + 11) : 0;
        if (::send(fd, packet, FrameHeaderSize + chunk, MSG_NOSIGNAL) !=
            static_cast<ssize_t>(FrameHeaderSize + chunk))
            return false;
        offset += chunk;
    }
    return true;
}

bool sendSmallFrame(int fd, quint64 channelId, quint64 sequence = 1)
{
    char packet[FrameHeaderSize + 16];
    std::memcpy(packet, "MGUI", 4);
    write32(packet + 4, 1);
    write64(packet + 8, channelId);
    write64(packet + 16, sequence);
    write32(packet + 24, 2);
    write32(packet + 28, 2);
    write32(packet + 32, 8);
    write32(packet + 36, 16);
    write32(packet + 40, 0);
    write32(packet + 44, 16);
    for (int i = 0; i < 16; ++i)
        packet[FrameHeaderSize + i] = static_cast<char>(i + 1);
    return ::send(fd, packet, sizeof(packet), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(packet));
}

int fakeWorker(bool rejectOpen, bool disconnectOpen, bool disconnectOnClose, bool exitAfterReady,
               bool sendFrames, bool sendFrameBeforeUi, bool sendKeyboard, bool receiveResize,
               bool sendBurst)
{
    struct Channel {
        quint64 id;
        int fd;
    };
    Channel channels[32];
    int channelCount = 0;
    if (!sendReply(3, Ready, 0)) return 21;
    if (exitAfterReady) {
        usleep(200000);
        return 0;
    }
    for (;;) {
        char packet[HeaderSize + 255];
        char control[CMSG_SPACE(sizeof(int) * 8)];
        struct iovec vector;
        vector.iov_base = packet;
        vector.iov_len = sizeof(packet);
        struct msghdr message = {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        const ssize_t count = ::recvmsg(3, &message, MSG_CMSG_CLOEXEC);
        if (count < HeaderSize || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
            std::memcmp(packet, "MGR1", 4) != 0) return 22;
        int receivedFd = -1;
        int receivedCount = 0;
        for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len != CMSG_LEN(sizeof(int))) return 23;
            std::memcpy(&receivedFd, CMSG_DATA(header), sizeof(receivedFd));
            ++receivedCount;
        }
        const quint32 type = read32(packet + 4);
        const quint64 channelId = read64(packet + 8);
        const quint32 uid = read32(packet + 16);
        const quint32 pid = read32(packet + 20);
        const quint32 packageLength = read32(packet + 24);
        if (type == Open) {
            if (count != HeaderSize + packageLength || receivedCount != 1 || !channelId ||
                uid != 12000 || pid != 50001 || packageLength != 15 ||
                std::memcmp(packet + HeaderSize, "org.example.app", packageLength) != 0) return 24;
            if (rejectOpen) {
                ::close(receivedFd);
                if (!sendReply(3, Nack, channelId)) return 25;
                continue;
            }
            if (channelCount == 32) return 26;
            channels[channelCount++] = Channel{channelId, receivedFd};
            if (sendFrameBeforeUi) {
                if (!sendSmallFrame(4, channelId)) return 39;
                if (!sendFrameDiagnostic(3, 18, channelId)) return 41;
                if (!sendReply(3, 8, channelId)) return 40;
            }
            if (sendBurst) {
                for (quint64 sequence = 1; sequence <= 64; ++sequence)
                    if (!sendSmallFrame(4, channelId, sequence)) return 49;
                if (!sendReply(3, 8, channelId)) return 50;
            }
            if (sendFrames) {
                if (!sendReply(3, 8, channelId)) return 36;
            }
            if ((sendKeyboard || receiveResize) && !sendReply(3, 8, channelId)) return 44;
            if (!sendReply(3, Ack, channelId)) return 27;
            if (sendKeyboard && !sendKeyboardShow(3, channelId, 5)) return 45;
            if (sendFrames) {
                if (!sendFrame(4, channelId, 1, true)) return 37;
                if (!sendFrameDiagnostic(3, 18, channelId)) return 42;
                if (!sendFrame(4, channelId, 2, false)) return 38;
                if (!sendFrameDiagnostic(3, 19, channelId)) return 43;
            }
            if (disconnectOpen) {
                if (!sendReply(3, Disconnected, channelId)) return 28;
                ::close(receivedFd);
                --channelCount;
            }
        } else if (type == Close) {
            if (count != HeaderSize || receivedCount || uid || pid || packageLength) return 29;
            for (int i = 0; i < channelCount; ++i) {
                if (channels[i].id == channelId) {
                    ::close(channels[i].fd);
                    channels[i] = channels[--channelCount];
                    break;
                }
            }
            if ((sendKeyboard || receiveResize) && !sendReply(3, 9, channelId)) return 46;
            if (disconnectOnClose && !sendReply(3, Disconnected, channelId)) return 34;
            if (!sendReply(3, Ack, channelId)) return 30;
            if (disconnectOnClose && !sendReply(3, Disconnected, channelId)) return 35;
        } else if (type == 21 && receiveResize) {
            if (count != HeaderSize + 8 || receivedCount || packageLength != 8 ||
                !((uid == 480 && pid == 800 && read32(packet + HeaderSize) == 160 &&
                   read32(packet + HeaderSize + 4) == 0) ||
                  (uid == 800 && pid == 480 && read32(packet + HeaderSize) == 160 &&
                   read32(packet + HeaderSize + 4) == 1)))
                return 47;
            if (!sendKeyboardShow(3, channelId, 5)) return 48;
        } else if (type == Stop) {
            if (count != HeaderSize || receivedCount || channelId || uid || pid || packageLength) return 31;
            for (int i = 0; i < channelCount; ++i) ::close(channels[i].fd);
            return sendReply(3, Ack, 0) ? 0 : 32;
        } else {
            if (receivedFd >= 0) ::close(receivedFd);
            return 33;
        }
    }
}

bool openTestChannel(SharedMicrogSupervisor *supervisor, quint64 id, int *clientFd,
                     QString *error)
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return false;
    if (!supervisor->openChannel(QStringLiteral("org.example.app"), 12000, 50001, id,
                                 sockets[0], error)) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return false;
    }
    ::close(sockets[0]);
    *clientFd = sockets[1];
    return true;
}

bool check(bool value, const char *description)
{
    if (!value) std::fprintf(stderr, "FAIL: %s\n", description);
    return value;
}
}

int main(int argc, char **argv)
{
    bool workerMode = false, rejectOpen = false, disconnectOpen = false, disconnectOnClose = false;
    bool sendFrames = false, sendFrameBeforeUi = false, sendKeyboard = false, receiveResize = false, sendBurst = false;
    bool exitAfterReady = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shared-microg-worker") == 0) workerMode = true;
        else if (std::strcmp(argv[i], "--fake-worker-reject") == 0) rejectOpen = true;
        else if (std::strcmp(argv[i], "--fake-worker-disconnect") == 0) disconnectOpen = true;
        else if (std::strcmp(argv[i], "--fake-worker-disconnect-on-close") == 0) disconnectOnClose = true;
        else if (std::strcmp(argv[i], "--fake-worker-exit-after-ready") == 0) exitAfterReady = true;
        else if (std::strcmp(argv[i], "--fake-worker-frames") == 0) sendFrames = true;
        else if (std::strcmp(argv[i], "--fake-worker-frame-before-ui") == 0) sendFrameBeforeUi = true;
        else if (std::strcmp(argv[i], "--fake-worker-keyboard") == 0) sendKeyboard = true;
        else if (std::strcmp(argv[i], "--fake-worker-resize") == 0) receiveResize = true;
        else if (std::strcmp(argv[i], "--fake-worker-burst") == 0) sendBurst = true;
    }
    if (workerMode) return fakeWorker(rejectOpen, disconnectOpen, disconnectOnClose, exitAfterReady,
                                      sendFrames, sendFrameBeforeUi, sendKeyboard, receiveResize, sendBurst);

    QCoreApplication app(argc, argv);
    qInstallMessageHandler(captureDiagnostics);
    const QString realWorker = QString::fromLocal8Bit(qgetenv("MEGLER_SHARED_MICROG_TEST_WORKER"));
    if (!realWorker.isEmpty()) {
        const QString apk = QString::fromLocal8Bit(qgetenv("MEGLER_SHARED_MICROG_TEST_APK"));
        const QString androidRoot = QString::fromLocal8Bit(qgetenv("MEGLER_SHARED_MICROG_TEST_ANDROID_ROOT"));
        if (apk.isEmpty() || androidRoot.isEmpty()) return 2;
        QTemporaryDir temporary;
        if (!temporary.isValid()) return 3;
        const QString dataDir = QDir(temporary.path()).filePath(QStringLiteral("data"));
        const QString appsDir = QDir(temporary.path()).filePath(QStringLiteral("apps"));
        if (!QDir().mkpath(dataDir) || !QDir().mkpath(appsDir)) return 4;
        QFile appsFile(QDir(appsDir).filePath(QStringLiteral("apps.txt")));
        if (!appsFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
            appsFile.write("example.client1\t\tClient\t1.0\t1\n") < 0)
            return 5;
        appsFile.close();

        SharedMicrogSupervisor supervisor;
        const QStringList workerArguments = QStringList()
            << QStringLiteral("--apk=") + apk
            << QStringLiteral("--data-dir=") + dataDir
            << QStringLiteral("--android-root=") + androidRoot
            << QStringLiteral("--apps-dir=") + appsDir;
        QString error;
        if (!supervisor.configure(realWorker, workerArguments, &error)) return 6;
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return 7;
        const bool opened = supervisor.openChannel(QStringLiteral("example.client1"), 11001, 50001,
                                                    1, sockets[0], &error);
        ::close(sockets[0]);
        if (opened) supervisor.closeChannel(1);
        char byte = 0;
        struct pollfd descriptor = { sockets[1], POLLIN | POLLHUP, 0 };
        const int ready = ::poll(&descriptor, 1, 15000);
        const ssize_t result = ready > 0 ? ::read(sockets[1], &byte, 1) : -1;
        ::close(sockets[1]);
        if (!opened || result != 0) {
            std::fprintf(stderr, "Real worker supervisor integration failed: %s\n", qPrintable(error));
            return 8;
        }
        std::puts("Real worker opened and closed through SharedMicrogSupervisor");
        return 0;
    }

    const QString program = QCoreApplication::applicationFilePath();
    QString error;
    bool ok = true;
    int clientFd = -1;
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList(), &error), "configure retained worker");
        ok &= check(supervisor.startRetained(&error), "start worker without clients");
        ok &= check(openTestChannel(&supervisor, 17, &clientFd, &error), "open retained worker channel");
        if (clientFd >= 0) {
            supervisor.closeChannel(17);
            ::close(clientFd);
            clientFd = -1;
            ok &= check(!openTestChannel(&supervisor, 17, &clientFd, &error),
                        "worker survives last channel and remembers its ID");
            ok &= check(openTestChannel(&supervisor, 18, &clientFd, &error),
                        "retained worker accepts another client");
            if (clientFd >= 0) {
                supervisor.closeChannel(18);
                ::close(clientFd);
                clientFd = -1;
            }
            supervisor.releaseRetention();
            ok &= check(openTestChannel(&supervisor, 17, &clientFd, &error),
                        "released worker stops and starts afresh");
            if (clientFd >= 0) {
                supervisor.closeChannel(17);
                ::close(clientFd);
                clientFd = -1;
            }
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-exit-after-ready"),
                                         &error), "configure exiting retained worker");
        bool died = false;
        QEventLoop loop;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::workerDied, &loop, [&]() {
            died = true;
            loop.quit();
        });
        const bool started = supervisor.startRetained(&error);
        ok &= check(started, "start exiting retained worker");
        if (started) {
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            loop.exec();
            ok &= check(died, "detect retained worker exit without clients");
            if (died)
                ok &= check(supervisor.startRetained(&error), "restart an exited retained worker");
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-reject"),
                                         &error), "configure rejecting fixture");
        int sockets[2];
        ok &= check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                    "create rejected channel");
        if (ok) {
            ok &= check(!supervisor.openChannel(QStringLiteral("org.example.app"), 12000, 50001,
                                                11, sockets[0], &error) && !error.isEmpty(),
                        "propagate worker NACK without claiming a channel");
            ::close(sockets[0]);
            ::close(sockets[1]);
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-disconnect-on-close"),
                                         &error), "configure worker fixture");
        clientFd = -1;
        ok &= check(openTestChannel(&supervisor, 12, &clientFd, &error),
                    "send OPEN with identity and one descriptor");
        int secondClientFd = -1;
        ok &= check(openTestChannel(&supervisor, 14, &secondClientFd, &error),
                    "open a second channel on the same worker");
        if (clientFd >= 0 && secondClientFd >= 0) {
            supervisor.closeChannel(12);
            char byte = 0;
            ok &= check(::read(clientFd, &byte, 1) == 0, "CLOSE shuts down the bridge descriptor");
            int sockets[2];
            ok &= check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                        "create duplicate channel");
            if (ok) {
                ok &= check(!supervisor.openChannel(QStringLiteral("org.example.app"), 12000, 50001,
                                                    12, sockets[0], &error) && !error.isEmpty(),
                            "reject channel ID reuse while worker is alive");
                ::close(sockets[0]);
                ::close(sockets[1]);
            }
            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
            int thirdClientFd = -1;
            ok &= check(openTestChannel(&supervisor, 15, &thirdClientFd, &error),
                        "ignore late DISCONNECTED after local CLOSE");
            if (thirdClientFd >= 0) {
                supervisor.closeChannel(15);
                ::close(thirdClientFd);
            }
            supervisor.closeChannel(14);
            ::close(clientFd);
            ::close(secondClientFd);
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-disconnect"),
                                         &error), "configure disconnect fixture");
        bool disconnected = false;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::channelDisconnected,
                         [&disconnected](quint64 channelId) { disconnected = channelId == 13; });
        clientFd = -1;
        ok &= check(openTestChannel(&supervisor, 13, &clientFd, &error),
                    "open channel before asynchronous disconnect");
        if (clientFd >= 0) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&supervisor, &SharedMicrogSupervisor::channelDisconnected,
                             &loop, &QEventLoop::quit);
            timeout.start(3000);
            loop.exec();
            char byte = 0;
            ok &= check(disconnected && ::read(clientFd, &byte, 1) == 0,
                        "DISCONNECTED revokes the local bridge");
            ::close(clientFd);
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-frame-before-ui"),
                                         &error), "configure frame-before-UiStarted fixture");
        int frameClientFd = -1;
        int frameCount = 0, uiStartedCount = 0;
        quint64 readyChannel = 0;
        quint64 nonzeroBytes = 0;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationUiChanged,
                         [&uiStartedCount](quint64, bool active) {
            if (active) ++uiStartedCount;
        });
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationFrameChanged,
                         [&frameCount, &readyChannel, &nonzeroBytes]
                         (quint64 channelId, const QImage &frame) {
            ++frameCount;
            if (!frame.isNull()) {
                readyChannel = channelId;
                const quint64 bytes = static_cast<quint64>(frame.width()) *
                                      static_cast<quint64>(frame.height()) * 4;
                const unsigned char *pixels = frame.constBits();
                for (quint64 i = 0; i < bytes; ++i)
                    if (pixels[i]) ++nonzeroBytes;
            }
        });
        ok &= check(openTestChannel(&supervisor, 20, &frameClientFd, &error),
                    "open frame-before-UiStarted channel");
        if (frameClientFd >= 0) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(3000);
            loop.exec();
            supervisor.closeChannel(20);
            ::close(frameClientFd);
        }
        ok &= check(uiStartedCount == 1, "UiStarted activates frame-before-UiStarted channel");
        ok &= check(frameCount == 1 && readyChannel == 20,
                    "saved frame is emitted after UI activation without another frame");
        ok &= check(nonzeroBytes > 0, "saved frame carries nonzero pixels");
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-frames"),
                                         &error), "configure frame fixture");
        int frameClientFd = -1;
        int frameCount = 0, readyCount = 0, uiStartedCount = 0;
        quint64 readyChannel = 0;
        quint64 nonzeroBytes = 0;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationUiChanged,
                         [&uiStartedCount](quint64, bool active) {
            if (active) ++uiStartedCount;
        });
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationFrameChanged,
                         [&frameCount, &readyCount, &readyChannel, &nonzeroBytes]
                         (quint64 channelId, const QImage &frame) {
            ++frameCount;
            if (!frame.isNull() && frame.width() == 800 && frame.height() == 480) {
                ++readyCount;
                readyChannel = channelId;
                for (int y = 0; y < frame.height(); ++y) {
                    const unsigned char *row = frame.constBits() + y * frame.bytesPerLine();
                    for (int x = 0; x < frame.width() * 4; ++x)
                        if (row[x]) ++nonzeroBytes;
                }
            }
        });
        ok &= check(openTestChannel(&supervisor, 19, &frameClientFd, &error),
                    "open frame fixture channel");
        if (frameClientFd >= 0) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(3000);
            loop.exec();
            supervisor.closeChannel(19);
            ::close(frameClientFd);
        }
        ok &= check(uiStartedCount == 1, "UiStarted becomes ready before frames");
        ok &= check(frameCount == 2 && readyCount == 2,
                    "nonzero and later-zero complete frames are assembled");
        ok &= check(readyChannel == 19, "assembled frames retain the active channel");
        ok &= check(nonzeroBytes > 0, "first complete frame carries nonzero pixels");
        ok &= check(sawFrameDelivered && sawFrameSuppressed,
                    "frame diagnostics are accepted before and after ACK");
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-keyboard"),
                                         &error), "configure keyboard fixture");
        ok &= check(supervisor.startRetained(&error), "start retained keyboard worker");
        int keyboardCount = 0, deathCount = 0;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::workerDied,
                         [&deathCount]() { ++deathCount; });
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationKeyboardRequested,
                         [&keyboardCount](quint64 channel, int kind, int flags, int enter) {
            if ((channel == 31 || channel == 32) && kind == 3 && flags == 128 && enter == 5)
                ++keyboardCount;
        });
        for (quint64 channel = 31; channel <= 32; ++channel) {
            int clientFd = -1;
            ok &= check(openTestChannel(&supervisor, channel, &clientFd, &error),
                        "open keyboard channel");
            if (clientFd >= 0) {
                QEventLoop loop;
                QTimer::singleShot(100, &loop, SLOT(quit()));
                loop.exec();
                supervisor.closeChannel(channel);
                ::close(clientFd);
            }
        }
        ok &= check(keyboardCount == 2, "keyboard show survives channel reopen");
        ok &= check(deathCount == 0, "keyboard show does not kill retained worker");
        supervisor.releaseRetention();
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-resize"),
                                         &error), "configure resize fixture");
        bool beforeAck = false;
        int received = 0, deaths = 0;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationUiChanged,
                         [&supervisor, &beforeAck](quint64 channel, bool active) {
            if (active) beforeAck = supervisor.sendUiResize(channel, 480, 800, 160, 0);
        });
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationKeyboardRequested,
                         [&received](quint64, int, int, int) { ++received; });
        QObject::connect(&supervisor, &SharedMicrogSupervisor::workerDied,
                         [&deaths]() { ++deaths; });
        int clientFd = -1;
        ok &= check(openTestChannel(&supervisor, 40, &clientFd, &error), "open resize channel");
        ok &= check(beforeAck, "send portrait resize during channel opening");
        ok &= check(supervisor.sendUiResize(40, 800, 480, 160, 1), "send landscape resize");
        ok &= check(!supervisor.sendUiResize(40, 2049, 480, 160, 1) &&
                    !supervisor.sendUiResize(40, 800, 480, 160, 4) &&
                    !supervisor.sendUiResize(41, 800, 480, 160, 1), "reject invalid resize");
        QEventLoop loop;
        QTimer::singleShot(100, &loop, SLOT(quit()));
        loop.exec();
        ok &= check(received == 2 && deaths == 0, "resize packets received without worker failure");
        if (clientFd >= 0) {
            supervisor.closeChannel(40);
            ::close(clientFd);
        }
    }
    {
        SharedMicrogSupervisor supervisor;
        ok &= check(supervisor.configure(program, QStringList() << QStringLiteral("--fake-worker-burst"),
                                         &error), "configure frame burst fixture");
        int frames = 0, timerAtFrame = 0;
        QObject::connect(&supervisor, &SharedMicrogSupervisor::authenticationFrameChanged,
                         [&supervisor, &frames, &timerAtFrame](quint64, const QImage &frame) {
            if (frame.isNull()) return;
            ++frames;
            if (frames == 1)
                QTimer::singleShot(0, &supervisor, [&frames, &timerAtFrame]() { timerAtFrame = frames; });
        });
        int clientFd = -1;
        ok &= check(openTestChannel(&supervisor, 41, &clientFd, &error), "open burst channel");
        QEventLoop loop;
        QTimer::singleShot(300, &loop, SLOT(quit()));
        loop.exec();
        ok &= check(frames == 64, "all queued frames are received");
        ok &= check(timerAtFrame > 0 && timerAtFrame < 64,
                    "frame burst yields to the GUI event loop");
        if (clientFd >= 0) {
            supervisor.closeChannel(41);
            ::close(clientFd);
        }
    }
    return ok ? 0 : 1;
}
