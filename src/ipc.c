#include "taskforge/ipc.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_all(int fd, const void* data, size_t len) {
    const char* p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int write_response(int fd, int ok, int error, const char* result, size_t length) {
    char header[128];
    int n = snprintf(header, sizeof(header), "%u %d %d %zu\n",
                     TASKFORGE_IPC_VERSION, ok, error, length);
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    /* Every response byte goes through MSG_NOSIGNAL, including error paths.
     * A library must not change the host process's SIGPIPE disposition. */
    if (write_all(fd, header, (size_t)n) != 0 ||
        write_all(fd, result, length) != 0) return -1;
    return write_all(fd, "\n", 1);
}

static int read_line(int fd, char* buf, size_t cap, uint32_t timeout_ms) {
    size_t n = 0;
    struct pollfd p = {fd, POLLIN, 0};
    while (n + 1 < cap) {
        int rc = poll(&p, 1, (int)timeout_ms);
        if (rc <= 0) return rc == 0 ? -2 : -1;
        char c;
        ssize_t got = recv(fd, &c, 1, 0);
        if (got <= 0) return -1;
        if (c == '\n') { buf[n] = 0; return 0; }
        buf[n++] = c;
    }
    return -3;
}

int taskforge_ipc_server_run(const char* path, const taskforge_ipc_handler_t* handlers,
                             size_t count, uint32_t max_requests) {
    if (!path || !handlers || count == 0 || strlen(path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return -1;
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return -1;
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(server, 16) != 0) { close(server); unlink(path); return -1; }
    uint32_t served = 0;
    while (!max_requests || served < max_requests) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; break; }
        char line[TASKFORGE_IPC_MAX_NAME + TASKFORGE_IPC_MAX_PAYLOAD + 64];
        int rc = read_line(client, line, sizeof(line), 30000);
        char name[TASKFORGE_IPC_MAX_NAME], *sep = NULL;
        if (rc == 0 && sscanf(line, "%u %63s", &(unsigned){0}, name) == 2 && (sep = strchr(line, ' '))) {
            sep = strchr(sep + 1, ' ');
            if (sep) {
                const char* arg = sep + 1; size_t arglen = strlen(arg);
                if (arglen <= TASKFORGE_IPC_MAX_PAYLOAD) {
                    for (size_t i = 0; i < count; i++) if (strcmp(name, handlers[i].name) == 0) {
                        char result[TASKFORGE_IPC_MAX_PAYLOAD + 1] = {0}; int err = 0;
                        int ok = handlers[i].handler(arg, arglen, result, sizeof(result), &err, handlers[i].context);
                        size_t length = strnlen(result, sizeof(result));
                        if (length == sizeof(result)) {
                            (void)write_response(client, 0, TASKFORGE_ERR_FAILED, "", 0);
                        } else {
                            (void)write_response(client, ok == 0, err, result, length);
                        }
                        goto served_request;
                    }
                }
            }
        }
        (void)write_response(client, 0, TASKFORGE_ERR_INVALID, "", 0);
served_request:
        close(client); served++;
    }
    close(server); unlink(path); return 0;
}

int taskforge_ipc_server_stop(const char* path) { return path ? unlink(path) : -1; }

int taskforge_ipc_client_call(const char* path, const char* name, const char* arg, size_t len,
                              char* result, size_t cap, int* task_error, uint32_t timeout_ms) {
    if (!path || !name || (!arg && len) || len > TASKFORGE_IPC_MAX_PAYLOAD || !result || cap == 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return -1;
    struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strncpy(a.sun_path, path, sizeof(a.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    char* request = malloc(len + TASKFORGE_IPC_MAX_NAME + 32); if (!request) { close(fd); return -1; }
    int n = snprintf(request, len + TASKFORGE_IPC_MAX_NAME + 32, "%u %s %.*s\n", TASKFORGE_IPC_VERSION, name, (int)len, arg ? arg : "");
    int rc = write_all(fd, request, (size_t)n); free(request); if (rc) { close(fd); return -1; }
    char header[128]; rc = read_line(fd, header, sizeof(header), timeout_ms); if (rc) { close(fd); return rc; }
    unsigned version, ok; int err; size_t length;
    if (sscanf(header, "%u %u %d %zu", &version, &ok, &err, &length) != 4 || version != TASKFORGE_IPC_VERSION || length >= cap) { close(fd); return -1; }
    char* body = malloc(length + 2); if (!body) { close(fd); return -1; }
    rc = read_line(fd, body, length + 2, timeout_ms); if (!rc) { memcpy(result, body, length); result[length] = 0; if (task_error) *task_error = err; }
    free(body); close(fd); return rc ? rc : (ok ? 0 : err);
}
#else
int taskforge_ipc_server_run(const char* p, const taskforge_ipc_handler_t* h, size_t n, uint32_t m) { (void)p;(void)h;(void)n;(void)m; return TASKFORGE_ERR_INVALID; }
int taskforge_ipc_server_stop(const char* p) { (void)p; return TASKFORGE_ERR_INVALID; }
int taskforge_ipc_client_call(const char* p,const char* n,const char* a,size_t l,char*r,size_t c,int*e,uint32_t t){(void)p;(void)n;(void)a;(void)l;(void)r;(void)c;(void)e;(void)t;return TASKFORGE_ERR_INVALID;}
#endif
