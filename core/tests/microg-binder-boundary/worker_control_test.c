#include <poll.h>

static int fake_frame_writable;
static int fake_frame_poll_count;
static int worker_test_poll(struct pollfd *fds, nfds_t count, int timeout);
#define poll worker_test_poll
#define main worker_program_main
#include "../../../aurora/shared_microg_worker.c"
#undef main
#undef poll

#include <assert.h>

static int worker_test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (fake_frame_writable && count == 1 && fds[0].fd == frame_fd && fds[0].events == POLLOUT) {
        ++fake_frame_poll_count;
        fds[0].revents = POLLOUT;
        return 1;
    }
    return poll(fds, count, timeout);
}

static char result[400];
static int last_touch_phase = -1, last_touch_pointer = -1, last_touch_x = -1, last_touch_y = -1;
static int key_events, text_keys, keyboard_closes, keyboard_height = -1;
static int surface_calls, surface_width, surface_height, surface_density, surface_rotation;
static char last_text[MAX_CONTROL_INPUT + 1];

struct frame_reader {
    int fd;
    const unsigned char *pixels;
    uint32_t width, height, stride, total;
    uint64_t sequence;
    int complete;
};

static void *read_frame_chunks_after(void *user, long delay_ns)
{
    struct frame_reader *reader = (struct frame_reader *)user;
    unsigned char packet[FRAME_HEADER_SIZE + FRAME_CHUNK];
    struct timespec delay = { 0, delay_ns };
    uint32_t offset = 0;
    if (delay_ns) nanosleep(&delay, NULL);
    while (offset < reader->total) {
        struct pollfd ready = { reader->fd, POLLIN, 0 };
        ssize_t size;
        uint32_t chunk, i;
        if (poll(&ready, 1, 3000) <= 0 || !(ready.revents & POLLIN)) return NULL;
        size = recv(reader->fd, packet, sizeof(packet), 0);
        if (size < FRAME_HEADER_SIZE || memcmp(packet, "MGUI", 4) ||
            read32(packet + 4) != 2 || read64(packet + 8) != UINT64_C(0x12345678) ||
            read64(packet + 48) != ui_generation ||
            read64(packet + 16) != reader->sequence || read32(packet + 24) != reader->width ||
            read32(packet + 28) != reader->height || read32(packet + 32) != reader->stride ||
            read32(packet + 36) != reader->total || read32(packet + 40) != offset)
            return NULL;
        chunk = read32(packet + 44);
        if (!chunk || chunk > FRAME_CHUNK ||
            size != FRAME_HEADER_SIZE + chunk || chunk > reader->total - offset)
            return NULL;
        for (i = 0; i < chunk; ++i)
            if (packet[FRAME_HEADER_SIZE + i] != reader->pixels[offset + i]) return NULL;
        offset += chunk;
    }
    reader->complete = 1;
    return NULL;
}

static void *read_frame_chunks(void *user)
{
    return read_frame_chunks_after(user, 100000000);
}

static void *read_frame_chunks_fast(void *user)
{
    return read_frame_chunks_after(user, 0);
}

static void benchmark_frame_send(void)
{
    const int width = 800, height = 480, stride = width * 4, iterations = 30;
    const uint32_t total = (uint32_t)(stride * height);
    unsigned char *pixels = (unsigned char *)malloc(total);
    int frames[2], send_buffer = 64 * 1024, frame;
    struct timespec started, finished;
    int64_t elapsed_us;
    struct frame_reader reader;
    pthread_t thread;
    assert(pixels);
    memset(pixels, 0x7f, total);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    assert(setsockopt(frames[1], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    reader.fd = frames[0];
    reader.pixels = pixels;
    reader.width = width;
    reader.height = height;
    reader.stride = stride;
    reader.total = total;
    frame_fd = frames[1];
    active_ui_channel = UINT64_C(0x12345678);
    ui_generation = frame_owner_generation = 1;
    frame_owner_channel = active_ui_channel;
    frame_sequence = 0;
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    for (frame = 0; frame < iterations; ++frame) {
        reader.sequence = (uint64_t)frame + 1;
        reader.complete = 0;
        assert(pthread_create(&thread, NULL, read_frame_chunks_fast, &reader) == 0);
        worker_present_pixels(NULL, pixels, width, height, stride);
        assert(pthread_join(thread, NULL) == 0 && reader.complete);
    }
    assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
    elapsed_us = (int64_t)(finished.tv_sec - started.tv_sec) * 1000000 +
                 (finished.tv_nsec - started.tv_nsec) / 1000;
    printf("Frame transport: %d frames in %lld ms\n", iterations,
           (long long)(elapsed_us / 1000));
    active_ui_channel = 0;
    frame_fd = -1;
    close(frames[0]);
    close(frames[1]);
    free(pixels);
}

static void test_frame_send_backpressure(void)
{
    const int width = 800, height = 480, stride = width * 4;
    const uint32_t total = (uint32_t)(stride * height);
    unsigned char *pixels = (unsigned char *)malloc(total);
    int frames[2], send_buffer = 64 * 1024;
    pthread_t reader_thread;
    struct frame_reader reader;
    uint32_t i;
    assert(pixels);
    for (i = 0; i < total; ++i) pixels[i] = (unsigned char)(i * 37 + 11);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    assert(setsockopt(frames[1], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    reader.fd = frames[0];
    reader.pixels = pixels;
    reader.width = width;
    reader.height = height;
    reader.stride = stride;
    reader.total = total;
    reader.sequence = 1;
    reader.complete = 0;
    frame_fd = frames[1];
    active_ui_channel = UINT64_C(0x12345678);
    ui_generation = frame_owner_generation = 1;
    frame_owner_channel = active_ui_channel;
    frame_sequence = 0;
    assert(pthread_create(&reader_thread, NULL, read_frame_chunks, &reader) == 0);
    worker_present_pixels(NULL, pixels, width, height, stride);
    assert(pthread_join(reader_thread, NULL) == 0);
    assert(reader.complete);
    active_ui_channel = 0;
    frame_fd = -1;
    close(frames[0]);
    close(frames[1]);
    free(pixels);
}

static void test_frame_send_timeout(void)
{
    const int width = 800, height = 480, stride = width * 4;
    const uint32_t total = (uint32_t)(stride * height);
    unsigned char *pixels = (unsigned char *)malloc(total);
    int frames[2], send_buffer = 64 * 1024;
    struct timespec started, finished, cpu_started, cpu_finished;
    int64_t elapsed_ms, cpu_ms;
    uint32_t i;
    assert(pixels);
    for (i = 0; i < total; ++i) pixels[i] = (unsigned char)(i * 37 + 11);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    assert(setsockopt(frames[1], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    frame_fd = frames[1];
    active_ui_channel = UINT64_C(0x12345678);
    ui_generation = frame_owner_generation = 1;
    frame_owner_channel = active_ui_channel;
    frame_sequence = 0;
    fake_frame_writable = 1;
    fake_frame_poll_count = 0;
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_started) == 0);
    worker_present_pixels(NULL, pixels, width, height, stride);
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_finished) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
    fake_frame_writable = 0;
    elapsed_ms = (int64_t)(finished.tv_sec - started.tv_sec) * 1000 +
                 (finished.tv_nsec - started.tv_nsec) / 1000000;
    cpu_ms = (int64_t)(cpu_finished.tv_sec - cpu_started.tv_sec) * 1000 +
             (cpu_finished.tv_nsec - cpu_started.tv_nsec) / 1000000;
    assert(elapsed_ms >= 800 && elapsed_ms < 3000);
    assert(fake_frame_poll_count > 1 && cpu_ms < 500);
    active_ui_channel = 0;
    frame_fd = -1;
    close(frames[0]);
    close(frames[1]);
    free(pixels);
}

static void test_empty_initial_frame_not_sent(void)
{
    const int width = 800, height = 480, stride = width * 4;
    const uint32_t total = (uint32_t)(stride * height);
    unsigned char *pixels = (unsigned char *)calloc(total, 1);
    int frames[2], send_buffer = 64 * 1024;
    struct pollfd ready;
    pthread_t reader_thread;
    struct frame_reader reader;
    uint32_t i;
    assert(pixels);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    assert(setsockopt(frames[1], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    frame_fd = frames[1];
    active_ui_channel = UINT64_C(0x12345678);
    ui_generation = frame_owner_generation = 1;
    frame_owner_channel = active_ui_channel;
    frame_sequence = 0;
    worker_present_pixels(NULL, pixels, width, height, stride);
    ready.fd = frames[0];
    ready.events = POLLIN;
    ready.revents = 0;
    assert(poll(&ready, 1, 100) == 0);

    for (i = 0; i < total; ++i) pixels[i] = (unsigned char)(i * 37 + 11);
    reader.fd = frames[0];
    reader.pixels = pixels;
    reader.width = width;
    reader.height = height;
    reader.stride = stride;
    reader.total = total;
    reader.sequence = 1;
    reader.complete = 0;
    assert(pthread_create(&reader_thread, NULL, read_frame_chunks, &reader) == 0);
    worker_present_pixels(NULL, pixels, width, height, stride);
    assert(pthread_join(reader_thread, NULL) == 0);
    assert(reader.complete);

    memset(pixels, 0, total);
    frame_sequence = 1;
    reader.sequence = 2;
    reader.complete = 0;
    assert(pthread_create(&reader_thread, NULL, read_frame_chunks, &reader) == 0);
    worker_present_pixels(NULL, pixels, width, height, stride);
    assert(pthread_join(reader_thread, NULL) == 0);
    assert(reader.complete);
    active_ui_channel = 0;
    frame_fd = -1;
    close(frames[0]);
    close(frames[1]);
    free(pixels);
}

static void test_frame_diagnostics(void)
{
    int controls[2], frames[2], send_buffer = 4096;
    unsigned char pixels[16] = {0}, packet[FRAME_HEADER_SIZE + 16];
    struct pollfd ready;
    struct timespec started, finished;
    int64_t elapsed_ms;
    const uint64_t channel = UINT64_C(0x12345678);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, controls) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    control_fd = controls[1];
    frame_fd = frames[1];
    active_ui_channel = channel;
    frame_sequence = 0;
    ++ui_generation;
    frame_owner_channel = channel;
    frame_owner_generation = ui_generation;

    worker_present_pixels(NULL, pixels, 2, 2, 8);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) == HEADER_SIZE);
    assert(read32(packet + 4) == FRAME_SUPPRESSED && read64(packet + 8) == channel);
    assert(read32(packet + 16) == 2 && read32(packet + 20) == 2 && read32(packet + 24) == 0);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) < 0 && errno == EAGAIN);

    memset(pixels, 0x7f, sizeof(pixels));
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) == FRAME_HEADER_SIZE + 16);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) == HEADER_SIZE);
    assert(read32(packet + 4) == FRAME_DELIVERED && read32(packet + 24) == 0);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) == FRAME_HEADER_SIZE + 16);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) < 0 && errno == EAGAIN);

    ++ui_generation;
    frame_owner_generation = ui_generation;
    assert(setsockopt(control_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    memset(packet, 0, HEADER_SIZE);
    while (send(control_fd, packet, HEADER_SIZE, MSG_DONTWAIT | MSG_NOSIGNAL) == HEADER_SIZE) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
    elapsed_ms = (finished.tv_sec - started.tv_sec) * 1000 +
                 (finished.tv_nsec - started.tv_nsec) / 1000000;
    assert(elapsed_ms < 500);
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) == FRAME_HEADER_SIZE + 16);
    while (recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(send_packet(ACK, channel));
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) == HEADER_SIZE);
    assert(read32(packet + 4) == ACK);

    ++ui_generation;
    frame_owner_generation = ui_generation;
    pthread_mutex_lock(&send_lock);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    pthread_mutex_unlock(&send_lock);
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) == FRAME_HEADER_SIZE + 16);
    ready.fd = controls[0];
    ready.events = POLLIN;
    ready.revents = 0;
    assert(poll(&ready, 1, 0) == 0);

    ++ui_generation;
    frame_owner_generation = ui_generation;
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) == FRAME_HEADER_SIZE + 16);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) == HEADER_SIZE);
    assert(read32(packet + 4) == FRAME_DELIVERED);

    ++ui_generation;
    frame_owner_generation = ui_generation;
    close(frames[0]);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) == HEADER_SIZE);
    assert(read32(packet + 4) == FRAME_SEND_FAILED && read32(packet + 16) == 0 &&
           read32(packet + 20) == sizeof(pixels) && read32(packet + 24) == 0);
    worker_present_pixels(NULL, pixels, 2, 2, 8);
    assert(recv(controls[0], packet, sizeof(packet), MSG_DONTWAIT) < 0 && errno == EAGAIN);

    active_ui_channel = 0;
    frame_fd = control_fd = -1;
    close(frames[1]);
    close(controls[0]);
    close(controls[1]);
}

static void test_frame_ownership(void)
{
    int controls[2], frames[2];
    unsigned char pixels[16], packet[FRAME_HEADER_SIZE + sizeof(pixels)];
    mg_jvalue args[2];
    mg_jcall call;
    uint64_t first, second;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, controls) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    memset(pixels, 0x7f, sizeof(pixels));
    control_fd = controls[1];
    frame_fd = frames[1];
    active_ui_channel = frame_owner_channel = frame_owner_generation = 0;
    ui_generation = frame_sequence = 0;
    first = start_ui(11);
    assert(first == 1 && activate_ui(11, first));
    assert(recv(controls[0], packet, sizeof(packet), 0) == HEADER_SIZE);
    assert(read64(packet + 16) == first);
    assert(worker_send_pixels(pixels, 2, 2, 8));
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) < 0 && errno == EAGAIN);

    memset(&call, 0, sizeof(call));
    memset(args, 0, sizeof(args));
    call.args = args;
    args[0].j = 11;
    args[1].j = first;
    worker_authorize_frame(&call);
    assert(call.ret.i);
    assert(worker_send_pixels(pixels, 2, 2, 8));
    assert(recv(frames[0], packet, sizeof(packet), 0) == FRAME_HEADER_SIZE + sizeof(pixels));
    assert(read64(packet + 8) == 11 && read64(packet + 48) == first);
    assert(finish_ui(11, first));
    assert(recv(controls[0], packet, sizeof(packet), 0) == HEADER_SIZE);
    assert(read32(packet + 4) == FRAME_DELIVERED);
    assert(recv(controls[0], packet, sizeof(packet), 0) == HEADER_SIZE);
    assert(read32(packet + 4) == UI_FINISHED && read64(packet + 16) == first);

    second = start_ui(22);
    assert(second == 2 && activate_ui(22, second));
    assert(recv(controls[0], packet, sizeof(packet), 0) == HEADER_SIZE);
    assert(!finish_ui(11, first) && !finish_ui(22, first));
    worker_authorize_frame(&call);
    assert(!call.ret.i);
    assert(worker_send_pixels(pixels, 2, 2, 8));
    assert(recv(frames[0], packet, sizeof(packet), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    args[0].j = 22;
    args[1].j = second;
    worker_authorize_frame(&call);
    assert(call.ret.i && worker_send_pixels(pixels, 2, 2, 8));
    assert(recv(frames[0], packet, sizeof(packet), 0) == FRAME_HEADER_SIZE + sizeof(pixels));
    assert(read64(packet + 8) == 22 && read64(packet + 48) == second);
    assert(finish_ui(22, second));
    control_fd = frame_fd = -1;
    close(controls[0]);
    close(controls[1]);
    close(frames[0]);
    close(frames[1]);
}

static void *send_old_frame(void *user)
{
    return (void *)(intptr_t)worker_send_pixels(user, 800, 480, 3200);
}

static void test_inflight_frame_keeps_owner(void)
{
    const uint32_t total = 800 * 480 * 4;
    unsigned char *pixels = (unsigned char *)malloc(total);
    unsigned char packet[FRAME_HEADER_SIZE + FRAME_CHUNK];
    int controls[2], frames[2], send_buffer = 64 * 1024;
    uint32_t offset = 0;
    uint64_t first, second;
    pthread_t sender;
    void *result;
    mg_jvalue args[2] = {{0}};
    mg_jcall call = {0};
    assert(pixels);
    memset(pixels, 0x7f, total);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, controls) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frames) == 0);
    assert(setsockopt(frames[1], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    control_fd = controls[1];
    frame_fd = frames[1];
    active_ui_channel = frame_owner_channel = frame_owner_generation = 0;
    ui_generation = frame_sequence = 0;
    first = start_ui(11);
    assert(first == 1 && activate_ui(11, first));
    call.args = args;
    args[0].j = 11;
    args[1].j = first;
    worker_authorize_frame(&call);
    assert(call.ret.i);
    assert(pthread_create(&sender, NULL, send_old_frame, pixels) == 0);
    while (offset < total) {
        ssize_t size = recv(frames[0], packet, sizeof(packet), 0);
        uint32_t chunk;
        assert(size > FRAME_HEADER_SIZE);
        chunk = read32(packet + 44);
        assert(size == FRAME_HEADER_SIZE + chunk && read32(packet + 40) == offset);
        assert(read64(packet + 8) == 11 && read64(packet + 48) == first);
        offset += chunk;
        if (offset == chunk) {
            assert(finish_ui(11, first));
            second = start_ui(22);
            assert(second == 2 && activate_ui(22, second));
        }
    }
    assert(pthread_join(sender, &result) == 0 && (intptr_t)result == 1);
    args[0].j = 22;
    args[1].j = second;
    worker_authorize_frame(&call);
    assert(call.ret.i && ui_matches(22, second));
    assert(worker_send_pixels(pixels, 2, 2, 8));
    assert(recv(frames[0], packet, sizeof(packet), 0) == FRAME_HEADER_SIZE + 16);
    assert(read64(packet + 8) == 22 && read64(packet + 48) == second);
    assert(finish_ui(22, second));
    control_fd = frame_fd = -1;
    close(controls[0]);
    close(controls[1]);
    close(frames[0]);
    close(frames[1]);
    free(pixels);
}

mg_jobj *mg_dvm_new_string(const char *text)
{
    snprintf(result, sizeof(result), "%s", text);
    return (mg_jobj *)result;
}

const char *mg_dvm_string_utf8(mg_jobj *object) { return (const char *)object; }
int mg_net_import_unix_stream(int fd) { return dup(fd); }

void megler_touch(int phase, int pointer, float x, float y)
{
    last_touch_phase = phase;
    last_touch_pointer = pointer;
    last_touch_x = (int)x;
    last_touch_y = (int)y;
}

void megler_key(int key, bool down)
{
    assert(key == 4);
    key_events += down ? 1 : 10;
}

void megler_text_commit(const char *text) { snprintf(last_text, sizeof(last_text), "%s", text); }
void megler_text_key(int key) { text_keys = key; }
void megler_keyboard_closed(void) { keyboard_closes++; }
void megler_keyboard_height(int pixels) { keyboard_height = pixels; }
void megler_set_surface(int width, int height, int density, int rotation)
{
    ++surface_calls;
    surface_width = width;
    surface_height = height;
    surface_density = density;
    surface_rotation = rotation;
}

static void send_payload(int fd, uint32_t type, uint64_t channel, uint32_t uid, uint32_t pid,
                         const void *payload, size_t size, const int *fds, size_t count)
{
    unsigned char packet[HEADER_SIZE + 8 + MAX_CONTROL_INPUT] = { 'M', 'G', 'R', '1' };
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int) * 2)]; } ancillary;
    struct iovec iov;
    struct msghdr message;
    write32(packet + 4, type);
    write64(packet + 8, channel);
    write32(packet + 16, uid);
    write32(packet + 20, pid);
    write32(packet + 24, (uint32_t)size);
    if (size) memcpy(packet + HEADER_SIZE, payload, size);
    memset(&message, 0, sizeof(message));
    memset(&ancillary, 0, sizeof(ancillary));
    iov.iov_base = packet;
    iov.iov_len = HEADER_SIZE + size;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (count) {
        struct cmsghdr *cmsg;
        assert(count <= 2);
        message.msg_control = ancillary.bytes;
        message.msg_controllen = CMSG_SPACE(count * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    }
    assert(sendmsg(fd, &message, 0) == (ssize_t)iov.iov_len);
}

static void send_request(int fd, uint32_t type, uint64_t channel, uint32_t uid, uint32_t pid,
                         const char *package, const int *fds, size_t count)
{
    send_payload(fd, type, channel, uid, pid, package, package ? strlen(package) : 0, fds, count);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        benchmark_frame_send();
        return 0;
    }
    int control[2], stream[2], imported;
    unsigned type, uid, pid;
    unsigned long long channel;
    char name[256];
    mg_jcall call;
    mg_jvalue args[6];
    unsigned char reply[HEADER_SIZE];
    megler_run_config run = {0};
    megler_gfx_host graphics = {0};
    configure_worker_graphics(&run, &graphics);
    assert(run.block_unowned_egl_layers && run.window.present_pixels == worker_present_pixels);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    control_fd = control[1];
    send_request(control[0], OPEN, UINT64_MAX, 11000, 50001, "example.client", &stream[0], 1);
    memset(&call, 0, sizeof(call));
    worker_receive(&call);
    assert(call.ret.l && sscanf(result, "%u %llx %u %u %d %255s", &type, &channel, &uid, &pid,
                                 &imported, name) == 6);
    assert(type == OPEN && channel == UINT64_MAX && uid == 11000 && pid == 50001 &&
           imported >= 0 && !strcmp(name, "example.client"));
    close(imported);
    assert(send_packet(ACK, UINT64_MAX));
    assert(recv(control[0], reply, sizeof(reply), 0) == HEADER_SIZE);
    assert(!memcmp(reply, "MGR1", 4) && read32(reply + 4) == ACK && read64(reply + 8) == UINT64_MAX);
    active_ui_channel = pending_ui_channel = pending_ui_generation = ui_generation = 0;
    memset(&call, 0, sizeof(call));
    memset(args, 0, sizeof(args));
    call.args = args;
    args[0].j = UINT64_MAX;
    worker_start_ui(&call);
    assert(call.ret.j == 1 && !active_ui_channel);
    assert(!start_ui(2));
    args[1].j = 1;
    worker_activate_ui(&call);
    assert(call.ret.i && recv(control[0], reply, sizeof(reply), 0) == HEADER_SIZE);
    assert(read32(reply + 4) == UI_STARTED && read64(reply + 8) == UINT64_MAX);
    assert(!start_ui(2));
    frame_owner_channel = active_ui_channel;
    frame_owner_generation = ui_generation;
    assert(ui_matches(UINT64_MAX, 1));
    {
        int enter;
        for (enter = MEGLER_ENTER_DONE; enter <= MEGLER_ENTER_PREVIOUS; ++enter) {
            worker_keyboard_show(NULL, MEGLER_KEYBOARD_EMAIL, MEGLER_KEYBOARD_F_AGAIN, enter);
            assert(recv(control[0], reply, sizeof(reply), MSG_TRUNC) == HEADER_SIZE);
            assert(read32(reply + 4) == KEYBOARD_SHOW && read64(reply + 8) == UINT64_MAX &&
                   read32(reply + 16) == MEGLER_KEYBOARD_EMAIL &&
                   read32(reply + 20) == MEGLER_KEYBOARD_F_AGAIN &&
                   read32(reply + 24) == (uint32_t)enter);
        }
    }

    unsigned char touch[16] = {0};
    write64(touch, 1);
    write32(touch + 8, 123);
    write32(touch + 12, 456);
    send_payload(control[0], TOUCH, UINT64_MAX, 0, 2, touch, sizeof(touch), NULL, 0);
    worker_receive(&call);
    assert(call.ret.l && !strcmp(result, "12 ffffffffffffffff 0000000000000001 0 2 123 456"));

    {
        unsigned char geometry[16] = {0};
        write64(geometry, 1);
        write32(geometry + 8, 240);
        write32(geometry + 12, 1);
        send_payload(control[0], UI_RESIZE, UINT64_MAX, 480, 800,
                     geometry, sizeof(geometry), NULL, 0);
        worker_receive(&call);
        assert(call.ret.l && !strcmp(result, "21 ffffffffffffffff 0000000000000001 480 800 240 1"));
        assert(surface_calls == 0);
        args[0].j = UINT64_MAX; args[1].j = 1;
        args[2].i = 480; args[3].i = 800; args[4].i = 240; args[5].i = 1;
        worker_resize(&call);
        assert(surface_calls == 1 && surface_width == 480 && surface_height == 800 &&
               surface_density == 240 && surface_rotation == 1);
        args[1].j = 2;
        worker_resize(&call);
        assert(surface_calls == 1);
    }
    unsigned char input[32] = {0};
    write64(input, 1);
    send_payload(control[0], BACK, UINT64_MAX, 0, 0, input, 8, NULL, 0);
    worker_receive(&call);
    assert(call.ret.l && !strcmp(result, "13 ffffffffffffffff 0000000000000001"));
    memcpy(input + 8, "private input", 13);
    send_payload(control[0], TEXT_COMMIT, UINT64_MAX, 0, 0, input, 21, NULL, 0);
    worker_receive(&call);
    assert(call.ret.l && !strcmp(result, "14 ffffffffffffffff 0000000000000001 private input"));
    args[0].j = UINT64_MAX;
    args[1].j = 1;
    args[2].i = 320;
    worker_keyboard_height(&call);
    assert(keyboard_height == 320);
    args[1].j = 2;
    args[2].i = 100;
    worker_keyboard_height(&call);
    assert(keyboard_height == 320);
    args[1].j = 1;
    worker_finish_ui(&call);
    assert(call.ret.i == 1 && recv(control[0], reply, sizeof(reply), 0) == HEADER_SIZE);
    assert(read32(reply + 4) == UI_FINISHED && read64(reply + 8) == UINT64_MAX);
    worker_keyboard_height(&call);
    assert(keyboard_height == 320);
    worker_start_ui(&call);
    assert(call.ret.j == 2);
    args[1].j = 1;
    worker_cancel_pending_ui(&call);
    assert(!call.ret.i && pending_ui_generation == 2);
    args[1].j = 2;
    worker_cancel_pending_ui(&call);
    assert(call.ret.i && !pending_ui_channel);
    assert(!activate_ui(UINT64_MAX, 2));
    worker_start_ui(&call);
    assert(call.ret.j == 3 && activate_ui(UINT64_MAX, 3));
    assert(recv(control[0], reply, sizeof(reply), 0) == HEADER_SIZE);
    args[1].j = 1;
    worker_finish_ui(&call);
    assert(!call.ret.i && ui_matches(UINT64_MAX, 3));
    worker_keyboard_height(&call);
    assert(keyboard_height == 320);
    args[1].j = 3;
    worker_cancel_pending_ui(&call);
    assert(!call.ret.i && ui_matches(UINT64_MAX, 3));
    worker_finish_ui(&call);
    assert(call.ret.i && recv(control[0], reply, sizeof(reply), 0) == HEADER_SIZE);

    send_request(control[0], CLOSE, 7, 0, 0, NULL, NULL, 0);
    worker_receive(&call);
    assert(call.ret.l && !strcmp(result, "3 0000000000000007"));
    send_request(control[0], STOP, 0, 0, 0, NULL, NULL, 0);
    worker_receive(&call);
    assert(call.ret.l && !strcmp(result, "4 0000000000000000"));
    close(control[0]);
    close(control[1]);

    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control) == 0);
    control_fd = control[1];
    {
        unsigned char geometry[16] = {0};
        write64(geometry, 1);
        write32(geometry + 8, 160);
        write32(geometry + 12, 4);
        send_payload(control[0], UI_RESIZE, 1, 480, 800,
                     geometry, sizeof(geometry), NULL, 0);
        worker_receive(&call);
        assert(!call.ret.l && surface_calls == 1);
    }
    close(control[0]);
    close(control[1]);

    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control) == 0);
    control_fd = control[1];
    send_request(control[0], OPEN, 1, 11000, 50001, "example.client", stream, 2);
    worker_receive(&call);
    assert(!call.ret.l);
    close(control[0]);
    close(control[1]);
    close(stream[0]);
    close(stream[1]);
    control_fd = -1;
    test_frame_send_backpressure();
    test_frame_send_timeout();
    test_empty_initial_frame_not_sent();
    test_frame_diagnostics();
    test_frame_ownership();
    test_inflight_frame_keeps_owner();
    puts("Shared microG control packet tests passed");
    return 0;
}
