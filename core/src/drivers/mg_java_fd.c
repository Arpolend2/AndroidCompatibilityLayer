#include "mg_java.h"
#include "../bionic/mg_fd.h"
#include "../bionic/mg_net.h"
#include "../dvm/mg_dvm_internal.h"
#include <stdio.h>

#if !defined(_WIN32) && !defined(MG_CORE_ELF)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct native_pipe { int fd; } native_pipe;

static i32 pipe_read(void *object, void *buffer, u32 size, bool nonblock)
{
    native_pipe *pipe = (native_pipe *)object;
    ssize_t result;
    (void)nonblock;
    result = read(pipe->fd, buffer, size);
    if (result < 0) mg_set_errno(errno);
    return (i32)result;
}

static i32 pipe_write(void *object, const void *buffer, u32 size, bool nonblock)
{
    native_pipe *pipe = (native_pipe *)object;
    sigset_t set, old, pending;
    struct timespec zero = {0, 0};
    ssize_t result;
    int saved, had_pending, rc;
    (void)nonblock;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    rc = pthread_sigmask(SIG_BLOCK, &set, &old);
    if (rc) { mg_set_errno(rc); return -1; }
    if (sigpending(&pending) != 0) {
        saved = errno;
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        mg_set_errno(saved);
        return -1;
    }
    had_pending = sigismember(&pending, SIGPIPE);
    result = write(pipe->fd, buffer, size);
    saved = errno;
    if (result < 0 && saved == EPIPE && !had_pending)
        while (sigtimedwait(&set, NULL, &zero) < 0 && errno == EINTR) {}
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (result < 0) mg_set_errno(saved);
    return (i32)result;
}

static int pipe_poll(void *object, int events)
{
    native_pipe *pipe = (native_pipe *)object;
    struct pollfd fd;
    int result;
    fd.fd = pipe->fd;
    fd.events = (short)events;
    fd.revents = 0;
    result = poll(&fd, 1, 0);
    if (result < 0) {
        mg_err("shared microG: host pipe poll failed (errno %d)", errno);
        return MG_POLLERR;
    }
    return result > 0 ? fd.revents : 0;
}

static int pipe_readable(void *object)
{
    native_pipe *pipe = (native_pipe *)object;
    int bytes = 0;
    if (ioctl(pipe->fd, FIONREAD, &bytes) != 0)
        mg_err("shared microG: host pipe FIONREAD failed (errno %d)", errno);
    return bytes;
}

static void pipe_nonblock(void *object, bool enabled)
{
    native_pipe *pipe = (native_pipe *)object;
    int flags = fcntl(pipe->fd, F_GETFL);
    if (flags < 0 || fcntl(pipe->fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
        mg_err("shared microG: could not set host pipe nonblocking mode (errno %d)", errno);
}

static void pipe_close(void *object)
{
    native_pipe *pipe = (native_pipe *)object;
    close(pipe->fd);
    free(pipe);
}

static const mg_fd_ops pipe_ops = {
    pipe_read, pipe_write, pipe_poll, pipe_readable, pipe_nonblock, pipe_close, NULL, 0010000u, false
};

static int import_pipe(int fd)
{
    struct stat st;
    native_pipe *pipe;
    int copy, flags, guest;
    if (fstat(fd, &st) != 0) { mg_set_errno(errno); return -1; }
    if (!S_ISFIFO(st.st_mode)) { mg_set_errno(GE_OPNOTSUPP); return -1; }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) { mg_set_errno(errno); return -1; }
    if (flags & O_PATH) { mg_set_errno(GE_OPNOTSUPP); return -1; }
    copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) { mg_set_errno(errno); return -1; }
    pipe = (native_pipe *)mg_alloc(sizeof(*pipe));
    pipe->fd = copy;
    guest = mg_fd_register_with(&pipe_ops, pipe, (flags & O_ACCMODE) |
        ((flags & O_NONBLOCK) ? 04000 : 0) | 02000000);
    if (guest < 0) pipe_close(pipe);
    return guest;
}

static int dup_pipe(int fd)
{
    void *held = NULL;
    native_pipe *pipe = (native_pipe *)mg_fd_ext_hold(fd, &pipe_ops, &held, NULL);
    int copy, saved;
    if (!pipe) {
        if (*mg_errno_ptr() == GE_NOTSOCK) mg_set_errno(GE_OPNOTSUPP);
        return -1;
    }
    copy = fcntl(pipe->fd, F_DUPFD_CLOEXEC, 0);
    saved = errno;
    mg_fd_let_go(held);
    if (copy < 0) mg_set_errno(saved);
    return copy;
}

int mg_java_fd_create_pipe(int fds[2])
{
    int native[2], saved;
    if (!fds) { mg_set_errno(GE_INVAL); return -1; }
    if (pipe2(native, O_CLOEXEC) != 0) { mg_set_errno(errno); return -1; }
    fds[0] = import_pipe(native[0]);
    fds[1] = fds[0] < 0 ? -1 : import_pipe(native[1]);
    saved = *mg_errno_ptr();
    close(native[0]); close(native[1]);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) mg_fd_close(fds[0]);
        mg_set_errno(saved);
        return -1;
    }
    return 0;
}

int mg_java_fd_create_socketpair(int type, int fds[2])
{
    int native[2], saved;
    if (!fds || (type != SOCK_STREAM && type != SOCK_SEQPACKET)) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, native) != 0) {
        mg_set_errno(errno);
        return -1;
    }
    fds[0] = type == SOCK_STREAM ? mg_net_import_unix_stream(native[0]) : mg_net_import_unix_seqpacket(native[0]);
    fds[1] = fds[0] < 0 ? -1 :
        (type == SOCK_STREAM ? mg_net_import_unix_stream(native[1]) : mg_net_import_unix_seqpacket(native[1]));
    saved = *mg_errno_ptr();
    close(native[0]); close(native[1]);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) mg_fd_close(fds[0]);
        mg_set_errno(saved);
        return -1;
    }
    return 0;
}

int mg_java_fd_send_marker(int stream, const int *fds, int count)
{
    union { struct cmsghdr align; char data[CMSG_SPACE(MG_JAVA_FD_MAX * sizeof(int))]; } control;
    struct msghdr msg;
    struct iovec iov;
    int native[MG_JAVA_FD_MAX], sock, i, saved = 0;
    char marker = 0;
    ssize_t sent;
    if (count < 0 || count > MG_JAVA_FD_MAX || (count && !fds)) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    sock = mg_net_dup_native_unix_stream(stream);
    if (sock < 0) return -1;
    for (i = 0; i < count; ++i) {
        native[i] = mg_fd_dup_native_regular(fds[i]);
        if (native[i] < 0 && *mg_errno_ptr() == GE_OPNOTSUPP) {
            native[i] = dup_pipe(fds[i]);
        }
        if (native[i] < 0 && *mg_errno_ptr() == GE_OPNOTSUPP) {
            native[i] = mg_net_dup_native_unix_socket(fds[i]);
            if (native[i] < 0 && *mg_errno_ptr() == GE_NOTSOCK)
                mg_set_errno(GE_OPNOTSUPP);
        }
        if (native[i] < 0) {
            saved = *mg_errno_ptr();
            goto out;
        }
    }
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &marker;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (count) {
        struct cmsghdr *cmsg;
        msg.msg_control = control.data;
        msg.msg_controllen = CMSG_SPACE((size_t)count * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN((size_t)count * sizeof(int));
        memcpy(CMSG_DATA(cmsg), native, (size_t)count * sizeof(int));
    }
    do { sent = sendmsg(sock, &msg, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    if (sent != 1) saved = sent < 0 ? errno : GE_IO;
out:
    while (i > 0) close(native[--i]);
    close(sock);
    if (saved) { mg_set_errno(saved); return -1; }
    return 0;
}

int mg_java_fd_receive_marker(int stream, int *fds, int *count)
{
    union { struct cmsghdr align; char data[CMSG_SPACE(MG_JAVA_FD_MAX * sizeof(int))]; } control;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *cmsg;
    int native[MG_JAVA_FD_MAX], sock, received = 0, imported = 0, saved = 0, i;
    char marker = 0;
    ssize_t got;
    if (!fds || !count) { mg_set_errno(GE_INVAL); return -1; }
    *count = 0;
    sock = mg_net_dup_native_unix_stream(stream);
    if (sock < 0) return -1;
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &marker;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data;
    msg.msg_controllen = sizeof(control.data);
    do { got = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC); } while (got < 0 && errno == EINTR);
    if (got < 0) saved = errno;
    else if (got == 0) { close(sock); return 0; }
    if (got > 0) {
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            size_t bytes;
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                cmsg->cmsg_len < CMSG_LEN(0)) { saved = GE_INVAL; continue; }
            bytes = cmsg->cmsg_len - CMSG_LEN(0);
            if (bytes % sizeof(int) || bytes / sizeof(int) > (size_t)(MG_JAVA_FD_MAX - received)) {
                saved = GE_INVAL;
                continue;
            }
            memcpy(native + received, CMSG_DATA(cmsg), bytes);
            received += (int)(bytes / sizeof(int));
        }
        if ((msg.msg_flags & MSG_CTRUNC) || marker != 0) saved = GE_INVAL;
    }
    if (!saved) {
        for (i = 0; i < received; ++i) {
            fds[i] = mg_fd_import_native_regular(native[i]);
            if (fds[i] < 0 && *mg_errno_ptr() == GE_OPNOTSUPP)
                fds[i] = import_pipe(native[i]);
            if (fds[i] < 0 && *mg_errno_ptr() == GE_OPNOTSUPP)
                fds[i] = mg_net_import_unix_stream(native[i]);
            if (fds[i] < 0 && *mg_errno_ptr() == GE_INVAL)
                fds[i] = mg_net_import_unix_seqpacket(native[i]);
            if (fds[i] < 0) { saved = *mg_errno_ptr(); break; }
            ++imported;
        }
    }
    for (i = 0; i < received; ++i) close(native[i]);
    close(sock);
    if (saved) {
        while (imported > 0) mg_fd_close(fds[--imported]);
        mg_set_errno(saved);
        return -1;
    }
    *count = received;
    return 1;
}
#endif

static void fd_available(mg_jcall *call)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    call->ret.z = 1;
#else
    call->ret.z = 0;
#endif
}

#if !defined(_WIN32) && !defined(MG_CORE_ELF)
static void fd_error(void)
{
    char message[80];
    snprintf(message, sizeof(message), "Shared descriptor operation failed (errno %d)", *mg_errno_ptr());
    mg_dvm_throw("java/io/IOException", "%s", message);
}

static void fd_return_array(mg_jcall *call, int *fds, int count)
{
    mg_jobj *array = mg_dvm_new_array("[I", count);
    int i;
    if (!array) {
        for (i = 0; i < count; ++i) mg_fd_close(fds[i]);
        return;
    }
    if (count) memcpy(array->u.arr.data, fds, (size_t)count * sizeof(int));
    call->ret.l = array;
}
#endif

static void fd_create_pipe(mg_jcall *call)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    int fds[2];
    if (mg_java_fd_create_pipe(fds) != 0) { fd_error(); return; }
    fd_return_array(call, fds, 2);
#else
    (void)call;
    mg_dvm_throw("java/io/IOException", "Host descriptor transport is unavailable");
#endif
}

static void fd_create_socketpair(mg_jcall *call)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    int fds[2];
    if (mg_java_fd_create_socketpair(call->args[0].i, fds) != 0) { fd_error(); return; }
    fd_return_array(call, fds, 2);
#else
    (void)call;
    mg_dvm_throw("java/io/IOException", "Host descriptor transport is unavailable");
#endif
}

static void fd_send_marker(mg_jcall *call)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    mg_jobj *array = call->args[1].l;
    if (!array || array->u.arr.len > MG_JAVA_FD_MAX) {
        mg_dvm_throw("java/io/IOException", "Invalid descriptor list");
        return;
    }
    if (mg_java_fd_send_marker(call->args[0].i, (const int *)array->u.arr.data, (int)array->u.arr.len) != 0)
        fd_error();
#else
    (void)call;
    mg_dvm_throw("java/io/IOException", "Host descriptor transport is unavailable");
#endif
}

static void fd_receive_marker(mg_jcall *call)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    int fds[MG_JAVA_FD_MAX], count = 0, result = mg_java_fd_receive_marker(call->args[0].i, fds, &count);
    if (result < 0) { fd_error(); return; }
    if (result == 0) { call->ret.l = NULL; return; }
    fd_return_array(call, fds, count);
#else
    (void)call;
    mg_dvm_throw("java/io/IOException", "Host descriptor transport is unavailable");
#endif
}

const mg_jhle *mg_java_shared_microg_fd_natives(void)
{
    static const mg_jhle natives[] = {
        { "megler/SharedMicrogFdTransport", "available", "()Z", fd_available },
        { "megler/SharedMicrogFdTransport", "createPipe", "()[I", fd_create_pipe },
        { "megler/SharedMicrogFdTransport", "createSocketPair", "(I)[I", fd_create_socketpair },
        { "megler/SharedMicrogFdTransport", "sendMarker", "(I[I)V", fd_send_marker },
        { "megler/SharedMicrogFdTransport", "receiveMarker", "(I)[I", fd_receive_marker },
        { NULL, NULL, NULL, NULL }
    };
    return natives;
}
