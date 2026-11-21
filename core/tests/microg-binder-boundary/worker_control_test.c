#define main worker_program_main
#include "../../../aurora/shared_microg_worker.c"
#undef main

#include <assert.h>

static char result[400];

mg_jobj *mg_dvm_new_string(const char *text)
{
    snprintf(result, sizeof(result), "%s", text);
    return (mg_jobj *)result;
}

int mg_net_import_unix_stream(int fd) { return dup(fd); }

static void send_request(int fd, uint32_t type, uint64_t channel, uint32_t uid, uint32_t pid,
                         const char *package, const int *fds, size_t count)
{
    unsigned char packet[HEADER_SIZE + MAX_PACKAGE] = { 'M', 'G', 'R', '1' };
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int) * 2)]; } ancillary;
    struct iovec iov;
    struct msghdr message;
    size_t size = package ? strlen(package) : 0;
    write32(packet + 4, type);
    write64(packet + 8, channel);
    write32(packet + 16, uid);
    write32(packet + 20, pid);
    write32(packet + 24, (uint32_t)size);
    if (size) memcpy(packet + HEADER_SIZE, package, size);
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

int main(void)
{
    int control[2], stream[2], imported;
    unsigned type, uid, pid;
    unsigned long long channel;
    char name[256];
    mg_jcall call;
    unsigned char reply[HEADER_SIZE];
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
    send_request(control[0], OPEN, 1, 11000, 50001, "example.client", stream, 2);
    worker_receive(&call);
    assert(!call.ret.l);
    close(control[0]);
    close(control[1]);
    close(stream[0]);
    close(stream[1]);
    puts("Shared microG control packet tests passed");
    return 0;
}
