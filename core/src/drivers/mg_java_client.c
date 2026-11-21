#include "mg_java.h"
#include "../bionic/mg_fd.h"
#include "../bionic/mg_net.h"
#include "../dvm/mg_dvm_internal.h"
#include "../host/mg_host.h"

#include <stdio.h>
#include <string.h>
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
#include <unistd.h>
#endif

static int client_fd = -1;
static int client_uid, client_pid;
static char client_package[256];
static bool client_enabled, client_consumed;

bool mg_java_set_shared_microg_client(int host_fd, int uid, int pid, const char *package)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    int error = 0;
    mg_sock *validated;
    const char *p;
    if (client_enabled || host_fd < 0 || uid < 11000 || uid > 19999 || pid < 50000 ||
        !package || !package[0] || strlen(package) >= sizeof(client_package)) return false;
    for (p = package; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return false;
    validated = mg_sock_dup_unix_stream(host_fd, &error);
    if (!validated) return false;
    mg_sock_close(validated);
    client_fd = host_fd;
    client_uid = uid;
    client_pid = pid;
    mg_strlcpy(client_package, package, sizeof(client_package));
    client_enabled = true;
    return true;
#else
    (void)host_fd; (void)uid; (void)pid; (void)package;
    return false;
#endif
}

void mg_java_close_shared_microg_client(void)
{
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    if (client_fd >= 0) close(client_fd);
#endif
    client_fd = -1;
}

static void enabled(mg_jcall *call) { call->ret.i = client_enabled ? 1 : 0; }

static void take_bootstrap(mg_jcall *call)
{
    call->ret.l = NULL;
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    if (client_enabled && !client_consumed && client_fd >= 0) {
        int guest_fd = mg_net_import_unix_stream(client_fd);
        char bootstrap[320];
        client_consumed = true;
        mg_java_close_shared_microg_client();
        if (guest_fd >= 0) {
            snprintf(bootstrap, sizeof(bootstrap), "%d %d %d %s",
                     client_uid, client_pid, guest_fd, client_package);
            call->ret.l = mg_dvm_new_string(bootstrap);
            if (!call->ret.l) mg_fd_close(guest_fd);
            return;
        }
    }
#endif
    mg_dvm_throw("java/lang/IllegalStateException", "Shared microG client bootstrap is unavailable");
}

const mg_jhle *mg_java_shared_microg_client_natives(void)
{
    static const mg_jhle natives[] = {
        { "megler/SharedMicrogClientStartup", "enabled", "()Z", enabled },
        { "megler/SharedMicrogClientStartup", "takeBootstrap", "()Ljava/lang/String;", take_bootstrap },
        { NULL, NULL, NULL, NULL }
    };
    return natives;
}
