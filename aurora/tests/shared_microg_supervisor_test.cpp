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
enum { HeaderSize = 28 };

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

int fakeWorker(bool rejectOpen, bool disconnectOpen, bool disconnectOnClose, bool exitAfterReady)
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
            if (!sendReply(3, Ack, channelId)) return 27;
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
            if (disconnectOnClose && !sendReply(3, Disconnected, channelId)) return 34;
            if (!sendReply(3, Ack, channelId)) return 30;
            if (disconnectOnClose && !sendReply(3, Disconnected, channelId)) return 35;
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
    bool exitAfterReady = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shared-microg-worker") == 0) workerMode = true;
        else if (std::strcmp(argv[i], "--fake-worker-reject") == 0) rejectOpen = true;
        else if (std::strcmp(argv[i], "--fake-worker-disconnect") == 0) disconnectOpen = true;
        else if (std::strcmp(argv[i], "--fake-worker-disconnect-on-close") == 0) disconnectOnClose = true;
        else if (std::strcmp(argv[i], "--fake-worker-exit-after-ready") == 0) exitAfterReady = true;
    }
    if (workerMode) return fakeWorker(rejectOpen, disconnectOpen, disconnectOnClose, exitAfterReady);

    QCoreApplication app(argc, argv);
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
    return ok ? 0 : 1;
}
