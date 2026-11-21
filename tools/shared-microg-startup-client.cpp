#include "../aurora/shared_microg_client.h"
extern "C" {
#include "../core/src/drivers/mg_java.h"
#include "../core/src/apk/mg_apk.h"
}

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct RuntimeResult {
    std::atomic<bool> ended{false};
    int status = -1;
};

void MEGLER_CB runtimeLog(int level, const char *line, void *)
{
    std::fprintf(level >= 3 ? stderr : stdout, "%s\n", line);
    std::fflush(level >= 3 ? stderr : stdout);
}

void MEGLER_CB runtimeExit(void *user, int status, const char *reason)
{
    RuntimeResult *result = static_cast<RuntimeResult *>(user);
    if (reason) std::fprintf(stderr, "SharedMicrogStartup: runtime exit: %s\n", reason);
    result->status = status;
    result->ended.store(true, std::memory_order_release);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--bootstrap-check") {
        int stream[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stream) != 0) {
            std::perror("SharedMicrogStartup: socketpair");
            return 1;
        }
        const bool rejected =
            !mg_java_set_shared_microg_client(stream[0], 10000, 50000, "example.client") &&
            !mg_java_set_shared_microg_client(stream[0], 20000, 50000, "example.client") &&
            !mg_java_set_shared_microg_client(stream[0], 11000, 1, "example.client") &&
            !mg_java_set_shared_microg_client(stream[0], 11000, 50000, "bad package") &&
            !mg_java_set_shared_microg_client(STDOUT_FILENO, 11000, 50000, "example.client");
        if (!rejected || ::fcntl(stream[0], F_GETFD) < 0 ||
            !mg_java_set_shared_microg_client(stream[0], 11000, 50000, "example.client") ||
            mg_java_set_shared_microg_client(stream[1], 11000, 50000, "example.client")) {
            ::close(stream[0]);
            ::close(stream[1]);
            std::fprintf(stderr, "FAIL native shared client validation or FD ownership\n");
            return 1;
        }
        mg_java_close_shared_microg_client();
        const bool closed = ::fcntl(stream[0], F_GETFD) < 0;
        ::close(stream[1]);
        if (!closed) {
            std::fprintf(stderr, "FAIL native shared client descriptor was not closed\n");
            return 1;
        }
        std::puts("PASS native shared client validation and FD ownership");
        return 0;
    }
    if (argc != 6) {
        std::fprintf(stderr, "Usage: shared-microg-startup-client APK DATA ANDROID_ROOT PICTURES WAIT_MS\n");
        return 2;
    }
    bool validWait = false;
    const int wait = QString::fromLocal8Bit(argv[5]).toInt(&validWait);
    if (!validWait || wait < 1 || wait > 120000) {
        std::fprintf(stderr, "SharedMicrogStartup: WAIT_MS must be between 1 and 120000\n");
        return 2;
    }
    mg_apk_info info = {};
    if (!mg_apk_read_info(argv[1], &info)) {
        std::fprintf(stderr, "SharedMicrogStartup: could not read APK: %s\n", info.error);
        return 2;
    }
    const QString package = QString::fromUtf8(info.package);
    mg_apk_info_free(&info);
    SharedMicrogClientChannel channel;
    QString error;
    if (!channel.openClient(package, &error)) {
        std::fprintf(stderr, "SharedMicrogStartup: channel open failed: %s\n", qPrintable(error));
        return 1;
    }
    const int fd = channel.takeStream(&error);
    const QByteArray packageBytes = package.toUtf8();
    if (fd < 0 || !mg_java_set_shared_microg_client(fd, channel.uid(), channel.pid(),
                                                   packageBytes.constData())) {
        if (fd >= 0) ::close(fd);
        std::fprintf(stderr, "SharedMicrogStartup: invalid trusted client bootstrap: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("SharedMicrogStartup: manager UID=%d PID=%d package=%s\n",
                channel.uid(), channel.pid(), packageBytes.constData());
    std::fflush(stdout);
    megler_config config = {};
    config.data_dir = argv[2];
    config.package_dir = argv[3];
    config.log = runtimeLog;
    if (!megler_init(&config)) {
        std::fprintf(stderr, "SharedMicrogStartup: runtime initialization failed: %s\n", megler_last_error());
        mg_java_close_shared_microg_client();
        return 1;
    }
    RuntimeResult result;
    megler_run_config run = {};
    run.apk_path = argv[1];
    run.android_root = argv[3];
    run.width = 800;
    run.height = 480;
    run.density_dpi = 160;
    run.on_exit = runtimeExit;
    run.user = &result;
    if (!megler_run_headless(&run)) {
        std::fprintf(stderr, "SharedMicrogStartup: guest startup failed: %s\n", megler_last_error());
        mg_java_close_shared_microg_client();
        return 1;
    }
    QEventLoop loop;
    QTimer exited;
    QObject::connect(&exited, &QTimer::timeout, &loop, [&]() {
        if (result.ended.load(std::memory_order_acquire)) loop.quit();
    });
    exited.start(20);
    QTimer::singleShot(wait, &loop, []() { megler_stop(); });
    QTimer::singleShot(wait + 15000, &loop, [&]() {
        if (!result.ended.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "FAIL SharedMicrogStartup: guest did not finish teardown\n");
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    });
    loop.exec();
    mg_java_close_shared_microg_client();
    // The guest's AppRunner finally completes teardown before dropping its authenticated sender.
    channel.close();
    return result.status == 0 ? 0 : 1;
}
