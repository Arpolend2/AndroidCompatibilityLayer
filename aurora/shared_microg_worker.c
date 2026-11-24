#include "../core/include/megler.h"
#include "../core/src/apk/mg_apk.h"
#include "../core/src/bionic/mg_net.h"
#include "../core/src/drivers/mg_java.h"
#include "../core/src/dvm/mg_dvm.h"
#include "shared_microg_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum { READY = 1, OPEN = 2, CLOSE = 3, STOP = 4, ACK = 5, NACK = 6, DISCONNECTED = 7,
       UI_STARTED = 8, UI_FINISHED = 9, KEYBOARD_SHOW = 10, KEYBOARD_HIDE = 11,
       TOUCH = 12, BACK = 13, TEXT_COMMIT = 14, TEXT_KEY = 15,
       KEYBOARD_CLOSED = 16, KEYBOARD_HEIGHT = 17,
       FRAME_DELIVERED = 18, FRAME_SUPPRESSED = 19, FRAME_SEND_FAILED = 20,
       UI_RESIZE = 21 };
enum { HEADER_SIZE = 28, MAX_PACKAGE = 255, MAX_CONTROL_INPUT = 4096,
       FRAME_HEADER_SIZE = 56, FRAME_CHUNK = 60 * 1024, MAX_FRAME_BYTES = 16 * 1024 * 1024,
       FRAME_SEND_TIMEOUT_MS = 1000 };
static int control_fd = -1;
static int frame_fd = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static int exited, exit_status;
static uint64_t active_ui_channel, ui_generation, frame_sequence;
static uint64_t pending_ui_channel, pending_ui_generation;
static uint64_t frame_owner_channel, frame_owner_generation;
/* Guarded by frame_lock; one best-effort diagnostic of each kind per UI. */
static uint64_t diagnostic_generation;
static unsigned frame_diagnostics;

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

static int send_packet_fields(unsigned type, uint64_t channel, uint32_t uid, uint32_t pid,
                              uint32_t third)
{
    unsigned char packet[HEADER_SIZE] = { 'M', 'G', 'R', '1' };
    const size_t packet_size = sizeof(packet);
    ssize_t sent;
    write32(packet + 4, type);
    write64(packet + 8, channel);
    write32(packet + 16, uid);
    write32(packet + 20, pid);
    write32(packet + 24, third);
    pthread_mutex_lock(&send_lock);
    do { sent = send(control_fd, packet, packet_size, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    pthread_mutex_unlock(&send_lock);
    if (sent != (ssize_t)packet_size) {
        fprintf(stderr, "shared microG: control send failed: %s\n", sent < 0 ? strerror(errno) : "short packet");
        shutdown(control_fd, SHUT_RDWR);
        return 0;
    }
    return 1;
}

static int send_packet(unsigned type, uint64_t channel)
{
    return send_packet_fields(type, channel, 0, 0, 0);
}

static int send_frame_diagnostic(unsigned type, uint64_t channel, uint32_t first, uint32_t second)
{
    unsigned char packet[HEADER_SIZE] = { 'M', 'G', 'R', '1' };
    unsigned bit = 1u << (type - FRAME_DELIVERED);
    if (frame_diagnostics & bit) return 0;
    frame_diagnostics |= bit;
    write32(packet + 4, type);
    write64(packet + 8, channel);
    write32(packet + 16, first);
    write32(packet + 20, second);
    if (pthread_mutex_trylock(&send_lock) != 0) return 1;
    if (control_fd >= 0)
        (void)send(control_fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&send_lock);
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
    unsigned char packet[HEADER_SIZE + 8 + MAX_CONTROL_INPUT];
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int) * 8)]; } ancillary;
    struct iovec iov = { packet, sizeof(packet) };
    struct msghdr message;
    struct cmsghdr *cmsg;
    int fds[8], count, invalid, guest_fd = -1;
    uint32_t type, uid, pid, package_size;
    uint64_t channel, generation = 0;
    ssize_t n;
    char text[HEADER_SIZE + MAX_CONTROL_INPUT + 96];
    count = 0;
    invalid = 0;
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
        if (type >= TOUCH && type <= KEYBOARD_HEIGHT) {
            if (package_size < 8 || (size_t)n != HEADER_SIZE + package_size) invalid = 1;
            else generation = read64(packet + HEADER_SIZE);
        } else if (type == UI_RESIZE) {
            if (package_size < 8 || (size_t)n != HEADER_SIZE + package_size) invalid = 1;
            else generation = read64(packet + HEADER_SIZE);
        }
        if (type == OPEN) {
            if (!channel || uid < 11000 || uid > 19999 || !pid || pid > INT32_MAX ||
                package_size > MAX_PACKAGE || (size_t)n != HEADER_SIZE + package_size ||
                count != 1 || !package_valid(packet + HEADER_SIZE, package_size)) invalid = 1;
        } else if (type == CLOSE || type == STOP) {
            if ((type == CLOSE && !channel) || (type == STOP && channel) ||
                uid || pid || package_size || n != HEADER_SIZE || count) invalid = 1;
        } else if (type == TOUCH) {
            if (!channel || !generation || uid > 2 || pid > 15 || package_size != 16 ||
                n != HEADER_SIZE + 16 || count ||
                read32(packet + HEADER_SIZE + 8) > 8192 ||
                read32(packet + HEADER_SIZE + 12) > 8192) invalid = 1;
        } else if (type == BACK || type == KEYBOARD_CLOSED) {
            if (!channel || !generation || uid || pid || package_size != 8 ||
                n != HEADER_SIZE + 8 || count) invalid = 1;
        } else if (type == TEXT_COMMIT) {
            if (!channel || !generation || uid || pid || package_size <= 8 || package_size > MAX_CONTROL_INPUT + 8 ||
                n != HEADER_SIZE + package_size || count ||
                memchr(packet + HEADER_SIZE + 8, 0, package_size - 8)) invalid = 1;
        } else if (type == TEXT_KEY || type == KEYBOARD_HEIGHT) {
            if (!channel || !generation || pid || package_size != 8 || n != HEADER_SIZE + 8 || count ||
                (type == TEXT_KEY && (uid < 1 || uid > 9)) ||
                (type == KEYBOARD_HEIGHT && uid > 8192)) invalid = 1;
        } else if (type == UI_RESIZE) {
            if (!channel || !generation || !uid || uid > 2048 || !pid || pid > 2048 ||
                package_size != 16 || n != HEADER_SIZE + 16 || count ||
                !read32(packet + HEADER_SIZE + 8) || read32(packet + HEADER_SIZE + 8) > 8192 ||
                read32(packet + HEADER_SIZE + 12) > 3) invalid = 1;
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
    } else if (type == TOUCH || type == UI_RESIZE) {
        snprintf(text, sizeof(text), "%u %016llx %016llx %u %u %u %u", type,
                 (unsigned long long)channel, (unsigned long long)generation, uid, pid,
                 read32(packet + HEADER_SIZE + 8), read32(packet + HEADER_SIZE + 12));
    } else if (type == TEXT_COMMIT) {
        snprintf(text, sizeof(text), "%u %016llx %016llx %.*s", type,
                 (unsigned long long)channel, (unsigned long long)generation,
                 (int)package_size - 8, (const char *)(packet + HEADER_SIZE + 8));
    } else if (type == TEXT_KEY || type == KEYBOARD_HEIGHT) {
        snprintf(text, sizeof(text), "%u %016llx %016llx %u", type,
                 (unsigned long long)channel, (unsigned long long)generation, uid);
    } else if (type == BACK || type == KEYBOARD_CLOSED) {
        snprintf(text, sizeof(text), "%u %016llx %016llx", type,
                 (unsigned long long)channel, (unsigned long long)generation);
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

static uint64_t start_ui(uint64_t channel)
{
    uint64_t generation = 0;
    pthread_mutex_lock(&ui_lock);
    if (channel && !active_ui_channel && !pending_ui_channel && ui_generation != UINT64_MAX) {
        generation = ++ui_generation;
        pending_ui_channel = channel;
        pending_ui_generation = generation;
    }
    pthread_mutex_unlock(&ui_lock);
    return generation;
}

static int activate_ui(uint64_t channel, uint64_t generation)
{
    int sent = 0;
    pthread_mutex_lock(&ui_lock);
    if (channel && generation && !active_ui_channel &&
        pending_ui_channel == channel && pending_ui_generation == generation) {
        pending_ui_channel = pending_ui_generation = 0;
        active_ui_channel = channel;
        frame_owner_channel = frame_owner_generation = 0;
        sent = send_packet_fields(UI_STARTED, channel, (uint32_t)(generation >> 32),
                                  (uint32_t)generation, 0);
        if (!sent) active_ui_channel = 0;
    }
    pthread_mutex_unlock(&ui_lock);
    return sent;
}

static int finish_ui(uint64_t channel, uint64_t generation)
{
    int sent = 0;
    pthread_mutex_lock(&ui_lock);
    if (channel && generation && pending_ui_channel == channel && pending_ui_generation == generation) {
        pending_ui_channel = pending_ui_generation = 0;
        sent = 1;
    } else if (channel && generation && active_ui_channel == channel && generation == ui_generation) {
        active_ui_channel = 0;
        frame_owner_channel = frame_owner_generation = 0;
        sent = send_packet_fields(UI_FINISHED, channel, (uint32_t)(ui_generation >> 32),
                                  (uint32_t)ui_generation, 0);
    }
    pthread_mutex_unlock(&ui_lock);
    return sent;
}

static void worker_start_ui(mg_jcall *call)
{
    call->ret.j = (int64_t)start_ui((uint64_t)call->args[0].j);
}

static void worker_activate_ui(mg_jcall *call)
{
    call->ret.i = activate_ui((uint64_t)call->args[0].j, (uint64_t)call->args[1].j);
}

static void worker_cancel_pending_ui(mg_jcall *call)
{
    const uint64_t channel = (uint64_t)call->args[0].j;
    const uint64_t generation = (uint64_t)call->args[1].j;
    pthread_mutex_lock(&ui_lock);
    call->ret.i = channel && generation && pending_ui_channel == channel &&
                  pending_ui_generation == generation;
    if (call->ret.i) pending_ui_channel = pending_ui_generation = 0;
    pthread_mutex_unlock(&ui_lock);
}

static void worker_finish_ui(mg_jcall *call)
{
    call->ret.i = finish_ui((uint64_t)call->args[0].j, (uint64_t)call->args[1].j);
}

static void worker_current_ui_channel(mg_jcall *call)
{
    pthread_mutex_lock(&ui_lock);
    call->ret.j = (int64_t)active_ui_channel;
    pthread_mutex_unlock(&ui_lock);
}

static void worker_current_ui_generation(mg_jcall *call)
{
    const uint64_t channel = (uint64_t)call->args[0].j;
    pthread_mutex_lock(&ui_lock);
    call->ret.j = channel && channel == active_ui_channel ? (int64_t)ui_generation : 0;
    pthread_mutex_unlock(&ui_lock);
}

static void worker_authorize_frame(mg_jcall *call)
{
    const uint64_t channel = (uint64_t)call->args[0].j;
    const uint64_t generation = (uint64_t)call->args[1].j;
    pthread_mutex_lock(&ui_lock);
    if (!channel && !generation) {
        frame_owner_channel = frame_owner_generation = 0;
        call->ret.i = 1;
    } else if (channel && generation && channel == active_ui_channel && generation == ui_generation) {
        frame_owner_channel = channel;
        frame_owner_generation = generation;
        call->ret.i = 1;
    } else {
        frame_owner_channel = frame_owner_generation = 0;
        call->ret.i = 0;
    }
    pthread_mutex_unlock(&ui_lock);
}

static int ui_matches(uint64_t channel, uint64_t generation)
{
    int matches;
    pthread_mutex_lock(&ui_lock);
    matches = channel && generation && active_ui_channel == channel && ui_generation == generation;
    pthread_mutex_unlock(&ui_lock);
    return matches;
}

static void worker_keyboard_height(mg_jcall *call)
{
    const int height = call->args[2].i;
    if (ui_matches((uint64_t)call->args[0].j, (uint64_t)call->args[1].j) &&
        height >= 0 && height <= 8192)
        megler_keyboard_height(height);
}

static void worker_resize(mg_jcall *call)
{
    const int width = call->args[2].i, height = call->args[3].i;
    const int density = call->args[4].i, rotation = call->args[5].i;
    if (ui_matches((uint64_t)call->args[0].j, (uint64_t)call->args[1].j) &&
        width > 0 && width <= 2048 && height > 0 && height <= 2048 &&
        density > 0 && density <= 8192 && rotation >= 0 && rotation <= 3)
        megler_set_surface(width, height, density, rotation);
}

static void MEGLER_CB worker_keyboard_show(void *user, int kind, int flags, int enter)
{
    uint64_t channel;
    (void)user;
    pthread_mutex_lock(&ui_lock);
    channel = active_ui_channel;
    pthread_mutex_unlock(&ui_lock);
    if (channel)
        send_packet_fields(KEYBOARD_SHOW, channel, (uint32_t)kind, (uint32_t)flags,
                           (uint32_t)enter);
}

static void MEGLER_CB worker_keyboard_hide(void *user)
{
    uint64_t channel;
    (void)user;
    pthread_mutex_lock(&ui_lock);
    channel = active_ui_channel;
    pthread_mutex_unlock(&ui_lock);
    if (channel) send_packet_fields(KEYBOARD_HIDE, channel, 0, 0, 0);
}

static int worker_send_pixels(const void *pixels, int width, int height, int stride)
{
    unsigned char packet[FRAME_HEADER_SIZE + FRAME_CHUNK];
    const unsigned char *source = (const unsigned char *)pixels;
    uint64_t channel, generation, sequence;
    uint32_t total, offset = 0;
    struct timespec deadline;
    if (!pixels || frame_fd < 0 || width <= 0 || width > 2048 || height <= 0 || height > 2048 ||
        stride < width * 4 || stride > width * 4 + 4096 ||
        (uint64_t)stride * (uint64_t)height > MAX_FRAME_BYTES)
        return 0;
    pthread_mutex_lock(&frame_lock);
    pthread_mutex_lock(&ui_lock);
    channel = frame_owner_channel;
    generation = frame_owner_generation;
    if (channel != active_ui_channel || generation != ui_generation) channel = 0;
    pthread_mutex_unlock(&ui_lock);
    if (!channel) { pthread_mutex_unlock(&frame_lock); return 1; }
    if (diagnostic_generation != generation) {
        diagnostic_generation = generation;
        frame_diagnostics = 0;
    }
    total = (uint32_t)(stride * height);
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        fprintf(stderr, "shared microG: cannot start UI frame send deadline: %s\n", strerror(errno));
        pthread_mutex_unlock(&frame_lock);
        return 0;
    }
    if (!frame_sequence) {
        uint32_t i;
        int nonzero = 0;
        for (i = 0; i < total && i < MAX_FRAME_BYTES; ++i) {
            if (source[i]) { nonzero = 1; break; }
        }
        if (!nonzero) {
            send_frame_diagnostic(FRAME_SUPPRESSED, channel, (uint32_t)width, (uint32_t)height);
            pthread_mutex_unlock(&frame_lock);
            return 1;
        }
    }
    deadline.tv_nsec += FRAME_SEND_TIMEOUT_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    sequence = ++frame_sequence;
    if (!sequence) sequence = ++frame_sequence;
    while (offset < total) {
        uint32_t chunk = total - offset;
        int send_error = 0;
        if (chunk > FRAME_CHUNK) chunk = FRAME_CHUNK;
        memcpy(packet, "MGUI", 4);
        write32(packet + 4, 2);
        write64(packet + 8, channel);
        write64(packet + 16, sequence);
        write32(packet + 24, (uint32_t)width);
        write32(packet + 28, (uint32_t)height);
        write32(packet + 32, (uint32_t)stride);
        write32(packet + 36, total);
        write32(packet + 40, offset);
        write32(packet + 44, chunk);
        write64(packet + 48, generation);
        memcpy(packet + FRAME_HEADER_SIZE, source + offset, chunk);
        int polledWritable = 0;
        for (;;) {
            struct pollfd writable = { frame_fd, POLLOUT, 0 };
            ssize_t sent;
            sent = send(frame_fd, packet, FRAME_HEADER_SIZE + chunk,
                        MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent == (ssize_t)(FRAME_HEADER_SIZE + chunk)) break;
            if (sent < 0 && errno == EINTR) continue;
            if (sent >= 0) {
                send_error = EIO;
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                send_error = errno;
                break;
            }
            if (polledWritable) {
                struct timespec retry_delay = { 0, 1000000L };
                nanosleep(&retry_delay, NULL);
                polledWritable = 0;
            }
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                send_error = errno;
                break;
            }
            int64_t remaining_ns = (int64_t)(deadline.tv_sec - now.tv_sec) * 1000000000LL +
                                   deadline.tv_nsec - now.tv_nsec;
            if (remaining_ns <= 0) {
                send_error = ETIMEDOUT;
                break;
            }
            int timeout_ms = (int)((remaining_ns + 999999LL) / 1000000LL);
            int ready;
            do { ready = poll(&writable, 1, timeout_ms); }
            while (ready < 0 && errno == EINTR);
            if (ready == 0) {
                send_error = ETIMEDOUT;
                break;
            }
            if (ready < 0) {
                send_error = errno;
                break;
            }
            if (writable.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                send_error = EPIPE;
                break;
            }
            if (!(writable.revents & POLLOUT)) {
                send_error = EIO;
                break;
            }
            polledWritable = 1;
        }
        if (send_error) {
            if (send_frame_diagnostic(FRAME_SEND_FAILED, channel, offset, total))
                fprintf(stderr, "shared microG: UI frame send failed at %u/%u bytes: %s\n",
                        offset, total, strerror(send_error));
            break;
        }
        offset += chunk;
    }
    if (offset == total) {
        send_frame_diagnostic(FRAME_DELIVERED, channel, (uint32_t)width, (uint32_t)height);
        if (sequence == 1 || sequence % 300 == 0) {
            uint32_t i, nonzero = 0;
            for (i = 0; i < total; ++i)
                if (source[i]) ++nonzero;
            fprintf(stderr, "shared microG: UI frame delivered: %ux%u, stride=%u, sequence=%llu, nonzero_bytes=%u/%u\n",
                    width, height, stride, (unsigned long long)sequence,
                    nonzero, total);
        }
    }
    pthread_mutex_unlock(&frame_lock);
    return offset == total;
}

static void MEGLER_CB worker_present_pixels(void *user, const void *pixels, int width, int height, int stride)
{
    (void)user;
    (void)worker_send_pixels(pixels, width, height, stride);
}

bool MEGLER_CB shared_microg_gpu_present_pixels(void *user, const void *pixels, int width, int height, int stride)
{
    (void)user;
    return worker_send_pixels(pixels, width, height, stride) != 0;
}

bool shared_microg_gpu_frame_owner(uint64_t *channel, uint64_t *generation)
{
    bool valid;
    if (!channel || !generation) return false;
    pthread_mutex_lock(&ui_lock);
    valid = frame_owner_channel != 0 && frame_owner_channel == active_ui_channel &&
            frame_owner_generation != 0 && frame_owner_generation == ui_generation;
    *channel = valid ? frame_owner_channel : 0;
    *generation = valid ? frame_owner_generation : 0;
    pthread_mutex_unlock(&ui_lock);
    return valid;
}

static void configure_worker_graphics(megler_run_config *run, const megler_gfx_host *graphics)
{
    run->gfx = *graphics;
    run->window.present_pixels = worker_present_pixels;
    run->block_unowned_egl_layers = true;
}

static const mg_jhle worker_natives[] = {
    { "megler/SharedMicrogWorker", "receive", "()Ljava/lang/String;", worker_receive },
    { "megler/SharedMicrogWorker", "send", "(IJ)Z", worker_send },
    { "megler/SharedMicrogWorker", "startUi", "(J)J", worker_start_ui },
    { "megler/SharedMicrogWorker", "activateUi", "(JJ)Z", worker_activate_ui },
    { "megler/SharedMicrogWorker", "cancelPendingUi", "(JJ)Z", worker_cancel_pending_ui },
    { "megler/SharedMicrogWorker", "finishUi", "(JJ)Z", worker_finish_ui },
    { "megler/SharedMicrogWorker", "currentUiChannel", "()J", worker_current_ui_channel },
    { "megler/SharedMicrogWorker", "currentUiGeneration", "(J)J", worker_current_ui_generation },
    { "megler/SharedMicrogWorker", "authorizeFrame", "(JJ)Z", worker_authorize_frame },
    { "megler/SharedMicrogWorker", "keyboardHeight", "(JJI)V", worker_keyboard_height },
    { "megler/SharedMicrogWorker", "resize", "(JJIIII)V", worker_resize },
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

int shared_microg_worker_main(int argc, char **argv, const megler_gfx_host *graphics)
{
    const char *apk = NULL, *data = NULL, *root = NULL, *apps = NULL;
    int headless = 0, version = 0, mode = 0, gpu = 0, i, type, frame_type;
    socklen_t type_size = sizeof(type), frame_type_size = sizeof(frame_type);
    struct sockaddr_storage local, peer, frame_local, frame_peer;
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
    socklen_t frame_local_size = sizeof(frame_local), frame_peer_size = sizeof(frame_peer);
    megler_config config;
    megler_run_config run;
    mg_apk_info info;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--shared-microg-worker") && !mode) mode = 1;
        else if (!strcmp(arg, "--gpu-canvas") && !gpu) gpu = 1;
        else if (!strcmp(arg, "--headless") && !headless) headless = 1;
        else if (!strcmp(arg, "--control-fd=3") && control_fd < 0) control_fd = 3;
        else if (!strcmp(arg, "--frame-fd=4") && frame_fd < 0) frame_fd = 4;
        else if (!strcmp(arg, "--protocol-version=1") && !version) version = 1;
        else if (!strncmp(arg, "--apk=", 6) && !apk) apk = arg + 6;
        else if (!strncmp(arg, "--data-dir=", 11) && !data) data = arg + 11;
        else if (!strncmp(arg, "--android-root=", 15) && !root) root = arg + 15;
        else if (!strncmp(arg, "--apps-dir=", 11) && !apps) apps = arg + 11;
        else { fprintf(stderr, "shared microG: invalid arguments\n"); return 1; }
    }
    if (!graphics || !graphics->canvas_new || !graphics->draw_text ||
        !headless || !version || control_fd != 3 || frame_fd != 4 ||
        getsockopt(control_fd, SOL_SOCKET, SO_TYPE, &type, &type_size) ||
        type != SOCK_SEQPACKET || getsockname(control_fd, (struct sockaddr *)&local, &local_size) ||
        getpeername(control_fd, (struct sockaddr *)&peer, &peer_size) ||
        local.ss_family != AF_UNIX || peer.ss_family != AF_UNIX ||
        getsockopt(frame_fd, SOL_SOCKET, SO_TYPE, &frame_type, &frame_type_size) ||
        frame_type != SOCK_SEQPACKET ||
        getsockname(frame_fd, (struct sockaddr *)&frame_local, &frame_local_size) ||
        getpeername(frame_fd, (struct sockaddr *)&frame_peer, &frame_peer_size) ||
        frame_local.ss_family != AF_UNIX || frame_peer.ss_family != AF_UNIX ||
        !absolute_path(apk, 0) || !absolute_path(data, 1) ||
        !absolute_path(root, 1) || !absolute_path(apps, 1)) {
        fprintf(stderr, "shared microG: invalid control socket or paths\n");
        return 1;
    }
    if (fcntl(control_fd, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(stderr, "shared microG: cannot secure control descriptor: %s\n", strerror(errno));
        return 1;
    }
    if (fcntl(frame_fd, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(stderr, "shared microG: cannot secure frame descriptor: %s\n", strerror(errno));
        close(control_fd);
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
    configure_worker_graphics(&run, graphics);
    run.keyboard.show = worker_keyboard_show;
    run.keyboard.hide = worker_keyboard_hide;
    run.on_exit = worker_exit;
    if (!megler_run_headless(&run)) {
        fprintf(stderr, "shared microG: startup failed: %s\n", megler_last_error());
        return 1;
    }
    pthread_mutex_lock(&exit_lock);
    while (!exited) pthread_cond_wait(&exit_cond, &exit_lock);
    pthread_mutex_unlock(&exit_lock);
    close(control_fd);
    close(frame_fd);
    return exit_status == 0 ? 0 : 1;
}

#ifdef MEGLER_SHARED_MICROG_WORKER_STANDALONE
#include "../core/tests/mg_gfx_standin.h"
int main(int argc, char **argv)
{
    return shared_microg_worker_main(argc, argv, mg_gfx_standin());
}
#endif
