#include "../core/include/megler.h"
#include "../core/src/apk/mg_apk.h"
#include "../core/src/bionic/mg_net.h"
#include "../core/src/drivers/mg_java.h"
#include "../core/src/dvm/mg_dvm.h"
#include "shared_microg_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { READY = 1, OPEN = 2, CLOSE = 3, STOP = 4, ACK = 5, NACK = 6, DISCONNECTED = 7 };
enum { HEADER_SIZE = 28, MAX_PACKAGE = 255 };
static int control_fd = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static int exited, exit_status;

static uint32_t read32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read64(const unsigned char *p)
{
    return ((uint64_t)read32(p) << 32) | read32(p + 4);
}

static void write32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void write64(unsigned char *p, uint64_t value)
{
    write32(p, (uint32_t)(value >> 32));
    write32(p + 4, (uint32_t)value);
}

static int send_packet(unsigned type, uint64_t channel)
{
    unsigned char packet[HEADER_SIZE] = { 'M', 'G', 'R', '1' };
    ssize_t sent;
    write32(packet + 4, type);
    write64(packet + 8, channel);
    pthread_mutex_lock(&send_lock);
    do { sent = send(control_fd, packet, sizeof(packet), MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    pthread_mutex_unlock(&send_lock);
    if (sent != sizeof(packet)) {
        fprintf(stderr, "shared microG: control send failed: %s\n", sent < 0 ? strerror(errno) : "short packet");
        shutdown(control_fd, SHUT_RDWR);
        return 0;
    }
    return 1;
}

static int package_valid(const unsigned char *name, size_t size)
{
    size_t i;
    int start = 1, dots = 0;
    if (!size || size > MAX_PACKAGE) return 0;
    for (i = 0; i < size; i++) {
        unsigned char c = name[i];
        if (c == '.') {
            if (start) return 0;
            start = 1;
            dots++;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                   (!start && c >= '0' && c <= '9')) {
            start = 0;
        } else return 0;
    }
    return !start && dots > 0;
}

static void worker_receive(mg_jcall *call)
{
    unsigned char packet[HEADER_SIZE + MAX_PACKAGE];
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int) * 8)]; } ancillary;
    struct iovec iov = { packet, sizeof(packet) };
    struct msghdr message;
    struct cmsghdr *cmsg;
    int fds[8], count = 0, invalid = 0, guest_fd = -1;
    uint32_t type, uid, pid, package_size;
    uint64_t channel;
    ssize_t n;
    char text[400];
    memset(&message, 0, sizeof(message));
    memset(&ancillary, 0, sizeof(ancillary));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.bytes;
    message.msg_controllen = sizeof(ancillary.bytes);
    do { n = recvmsg(control_fd, &message, MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        if (n < 0) fprintf(stderr, "shared microG: control receive failed: %s\n", strerror(errno));
        call->ret.l = NULL;
        return;
    }
    for (cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        size_t bytes, i;
        int *received;
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) { invalid = 1; continue; }
        bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) invalid = 1;
        received = (int *)CMSG_DATA(cmsg);
        for (i = 0; i < bytes / sizeof(int); i++) {
            if (count < 8) fds[count++] = received[i];
            else { close(received[i]); invalid = 1; }
        }
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n < HEADER_SIZE ||
        memcmp(packet, "MGR1", 4)) invalid = 1;
    if (!invalid) {
        type = read32(packet + 4);
        channel = read64(packet + 8);
        uid = read32(packet + 16);
        pid = read32(packet + 20);
        package_size = read32(packet + 24);
        if (type == OPEN) {
            if (!channel || uid < 11000 || uid > 19999 || !pid || pid > INT32_MAX ||
                package_size > MAX_PACKAGE || (size_t)n != HEADER_SIZE + package_size ||
                count != 1 || !package_valid(packet + HEADER_SIZE, package_size)) invalid = 1;
        } else if (type == CLOSE || type == STOP) {
            if ((type == CLOSE && !channel) || (type == STOP && channel) ||
                uid || pid || package_size || n != HEADER_SIZE || count) invalid = 1;
        } else invalid = 1;
    }
    if (invalid) {
        int i;
        for (i = 0; i < count; i++) close(fds[i]);
        fprintf(stderr, "shared microG: invalid control packet or descriptors\n");
        shutdown(control_fd, SHUT_RDWR);
        call->ret.l = NULL;
        return;
    }
    if (type == OPEN) {
        guest_fd = mg_net_import_unix_stream(fds[0]);
        close(fds[0]);
        if (guest_fd < 0) fprintf(stderr, "shared microG: cannot import channel socket\n");
        snprintf(text, sizeof(text), "%u %016llx %u %u %d %.*s", type,
                 (unsigned long long)channel, uid, pid, guest_fd, (int)package_size,
                 (const char *)(packet + HEADER_SIZE));
    } else {
        snprintf(text, sizeof(text), "%u %016llx", type, (unsigned long long)channel);
    }
    call->ret.l = mg_dvm_new_string(text);
}

static void worker_send(mg_jcall *call)
{
    int type = call->args[0].i;
    uint64_t channel = (uint64_t)call->args[1].j;
    if ((type != READY && type != ACK && type != NACK && type != DISCONNECTED) ||
        (type == READY && channel) || (type == DISCONNECTED && !channel)) {
        call->ret.i = 0;
        return;
    }
    call->ret.i = send_packet((unsigned)type, channel);
}

static const mg_jhle worker_natives[] = {
    { "megler/SharedMicrogWorker", "receive", "()Ljava/lang/String;", worker_receive },
    { "megler/SharedMicrogWorker", "send", "(IJ)Z", worker_send },
    { NULL, NULL, NULL, NULL }
};

static void MEGLER_CB worker_exit(void *user, int status, const char *reason)
{
    (void)user;
    if (reason) fprintf(stderr, "shared microG: runtime stopped: %s\n", reason);
    pthread_mutex_lock(&exit_lock);
    exit_status = status;
    exited = 1;
    pthread_cond_signal(&exit_cond);
    pthread_mutex_unlock(&exit_lock);
}

static void MEGLER_CB worker_log(int level, const char *line, void *user)
{
    (void)level;
    (void)user;
    fprintf(stderr, "%s\n", line);
}

static int absolute_path(const char *path, int directory)
{
    struct stat info;
    return path && path[0] == '/' && !stat(path, &info) &&
           (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
}

int shared_microg_worker_main(int argc, char **argv)
{
    const char *apk = NULL, *data = NULL, *root = NULL, *apps = NULL;
    int headless = 0, version = 0, mode = 0, i, type;
    socklen_t type_size = sizeof(type);
    struct sockaddr_storage local, peer;
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
    megler_config config;
    megler_run_config run;
    mg_apk_info info;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--shared-microg-worker") && !mode) mode = 1;
        else if (!strcmp(arg, "--headless") && !headless) headless = 1;
        else if (!strcmp(arg, "--control-fd=3") && control_fd < 0) control_fd = 3;
        else if (!strcmp(arg, "--protocol-version=1") && !version) version = 1;
        else if (!strncmp(arg, "--apk=", 6) && !apk) apk = arg + 6;
        else if (!strncmp(arg, "--data-dir=", 11) && !data) data = arg + 11;
        else if (!strncmp(arg, "--android-root=", 15) && !root) root = arg + 15;
        else if (!strncmp(arg, "--apps-dir=", 11) && !apps) apps = arg + 11;
        else { fprintf(stderr, "shared microG: invalid arguments\n"); return 1; }
    }
    if (!headless || !version || control_fd != 3 ||
        getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &type_size) ||
        type != SOCK_SEQPACKET || getsockname(control_fd, (struct sockaddr *)&local, &local_size) ||
        getpeername(control_fd, (struct sockaddr *)&peer, &peer_size) ||
        local.ss_family != AF_UNIX || peer.ss_family != AF_UNIX ||
        !absolute_path(apk, 0) || !absolute_path(data, 1) ||
        !absolute_path(root, 1) || !absolute_path(apps, 1)) {
        fprintf(stderr, "shared microG: invalid control socket or paths\n");
        return 1;
    }
    if (fcntl(control_fd, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(stderr, "shared microG: cannot secure control descriptor: %s\n", strerror(errno));
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.data_dir = data;
    config.package_dir = root;
    config.log = worker_log;
    if (!megler_init(&config)) { fprintf(stderr, "shared microG: core initialization failed\n"); return 1; }
    memset(&info, 0, sizeof(info));
    if (!mg_apk_read_info(apk, &info)) {
        fprintf(stderr, "shared microG: cannot read APK: %s\n", info.error);
        return 1;
    }
    if (strcmp(info.package, "com.google.android.gms") || !mg_apk_is_microg(apk)) {
        fprintf(stderr, "shared microG: APK is not the supported microG package\n");
        mg_apk_info_free(&info);
        return 1;
    }
    mg_apk_info_free(&info);
    mg_java_enable_shared_microg_worker();
    mg_dvm_register(worker_natives);
    memset(&run, 0, sizeof(run));
    run.apk_path = apk;
    run.android_root = root;
    run.apps_dir = apps;
    run.width = 800;
    run.height = 480;
    run.density_dpi = 160;
    run.on_exit = worker_exit;
    if (!megler_run_headless(&run)) {
        fprintf(stderr, "shared microG: startup failed: %s\n", megler_last_error());
        return 1;
    }
    pthread_mutex_lock(&exit_lock);
    while (!exited) pthread_cond_wait(&exit_cond, &exit_lock);
    pthread_mutex_unlock(&exit_lock);
    close(control_fd);
    return exit_status == 0 ? 0 : 1;
}

#ifdef MEGLER_SHARED_MICROG_WORKER_STANDALONE
int main(int argc, char **argv) { return shared_microg_worker_main(argc, argv); }
#endif
