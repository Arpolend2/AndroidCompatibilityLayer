#include "mg_store_restart.h"

#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#endif

bool mg_store_restart_request(const char *package)
{
#if defined(__linux__)
    const char *path = getenv("ANDROID_COMPATIBILITY_LAYER_STORE_RESTART_SOCKET");
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    struct timeval timeout = { 2, 0 };
    char reply = 0;
    int fd;
    bool accepted;
    if (!package || strcmp(package, "com.aurora.store") || !path || !*path ||
        strlen(path) >= sizeof(address.sun_path)) return false;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    accepted = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 &&
        peer_size == sizeof(peer) && peer.pid == getppid() &&
        send(fd, "R", 1, MSG_NOSIGNAL) == 1 && recv(fd, &reply, 1, 0) == 1 && reply == 'A';
    close(fd);
    return accepted;
#else
    (void)package;
    return false;
#endif
}
