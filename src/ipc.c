#include "taskforge/ipc.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
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

static int deadline_after(uint32_t timeout_ms, struct timespec* deadline) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return -1;
    deadline->tv_sec += timeout_ms / 1000U;
    deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int wait_readable_until(int fd, const struct timespec* deadline) {
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        int64_t remaining_ns =
            (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
            (int64_t)deadline->tv_nsec - now.tv_nsec;
        if (remaining_ns <= 0) return -2;

        int64_t remaining_ms = (remaining_ns + 999999LL) / 1000000LL;
        if (remaining_ms > INT_MAX) remaining_ms = INT_MAX;
        struct pollfd p = {fd, POLLIN, 0};
        int rc = poll(&p, 1, (int)remaining_ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (rc == 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        if (p.revents & (POLLIN | POLLHUP)) return 0;
    }
}

static int read_line_until(int fd, char* buf, size_t cap,
                           const struct timespec* deadline) {
    if (!buf || cap == 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        int rc = wait_readable_until(fd, deadline);
        if (rc != 0) return rc;
        char ch;
        ssize_t got = recv(fd, &ch, 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        if (ch == '\n') { buf[n] = '\0'; return 0; }
        buf[n++] = ch;
    }
    return -3;
}

static int read_line(int fd, char* buf, size_t cap, uint32_t timeout_ms) {
    struct timespec deadline;
    if (deadline_after(timeout_ms, &deadline) != 0) return -1;
    return read_line_until(fd, buf, cap, &deadline);
}

static int read_exact_until(int fd, void* buffer, size_t length,
                            const struct timespec* deadline) {
    unsigned char* out = buffer;
    size_t offset = 0;
    while (offset < length) {
        int rc = wait_readable_until(fd, deadline);
        if (rc != 0) return rc;
        ssize_t got = recv(fd, out + offset, length - offset, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        offset += (size_t)got;
    }
    return 0;
}

static int unlink_socket_if_same(const char* path, const struct stat* expected) {
    struct stat current;
    if (lstat(path, &current) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(current.st_mode) || current.st_dev != expected->st_dev ||
        current.st_ino != expected->st_ino) return -1;
    return unlink(path);
}

int taskforge_ipc_server_run(const char* path, const taskforge_ipc_handler_t* handlers,
                             size_t count, uint32_t max_requests) {
    if (!path || !*path || !handlers || count == 0 ||
        strlen(path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!handlers[i].name || !*handlers[i].name || !handlers[i].handler ||
            strlen(handlers[i].name) >= TASKFORGE_IPC_MAX_NAME ||
            strpbrk(handlers[i].name, " \t\r\n") != NULL) return -1;
    }
    /* Never unlink a caller-supplied path: it may be a regular file or
     * another live server's socket. Existing paths, including stale sockets,
     * must be removed explicitly by the owner before starting a new server. */
    struct stat existing;
    if (lstat(path, &existing) == 0 || errno != ENOENT) return -1;

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return -1;
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(server);
        return -1;
    }
    struct stat bound_path;
    if (lstat(path, &bound_path) != 0 || !S_ISSOCK(bound_path.st_mode)) {
        close(server);
        return -1;
    }
    if (listen(server, 16) != 0) {
        close(server);
        (void)unlink_socket_if_same(path, &bound_path);
        return -1;
    }
    uint32_t served = 0;
    while (!max_requests || served < max_requests) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; break; }
        char line[TASKFORGE_IPC_MAX_NAME + TASKFORGE_IPC_MAX_PAYLOAD + 64];
        int rc = read_line(client, line, sizeof(line), 30000);
        char name[TASKFORGE_IPC_MAX_NAME] = {0};
        unsigned version = 0;
        int consumed = 0;
        if (rc == 0 && sscanf(line, "%u %63s%n", &version, name, &consumed) == 2 &&
            version == TASKFORGE_IPC_VERSION && consumed > 0 &&
            line[consumed] == ' ') {
            const char* arg = line + consumed + 1;
            size_t arglen = strlen(arg);
            if (arglen <= TASKFORGE_IPC_MAX_PAYLOAD) {
                for (size_t i = 0; i < count; i++) {
                    if (handlers[i].name && handlers[i].handler &&
                        strcmp(name, handlers[i].name) == 0) {
                        char result[TASKFORGE_IPC_MAX_PAYLOAD + 1] = {0};
                        int err = 0;
                        int ok = handlers[i].handler(arg, arglen, result, sizeof(result),
                                                     &err, handlers[i].context);
                        if (ok != 0 && err == 0) err = TASKFORGE_ERR_FAILED;
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
    close(server);
    (void)unlink_socket_if_same(path, &bound_path);
    return 0;
}

int taskforge_ipc_server_stop(const char* path) {
    if (!path || !*path) return -1;
    struct stat current;
    if (lstat(path, &current) != 0 || !S_ISSOCK(current.st_mode)) return -1;
    return unlink(path);
}

int taskforge_ipc_client_call(const char* path, const char* name, const char* arg, size_t len,
                              char* result, size_t cap, int* task_error, uint32_t timeout_ms) {
    if (!path || !*path || !name || !*name || (!arg && len) ||
        len > TASKFORGE_IPC_MAX_PAYLOAD || !result || cap == 0 ||
        strlen(path) >= sizeof(((struct sockaddr_un*)0)->sun_path) ||
        strlen(name) >= TASKFORGE_IPC_MAX_NAME ||
        strpbrk(name, " \t\r\n") != NULL) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return -1;
    struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strncpy(a.sun_path, path, sizeof(a.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
    char* request = malloc(len + TASKFORGE_IPC_MAX_NAME + 32); if (!request) { close(fd); return -1; }
    int n = snprintf(request, len + TASKFORGE_IPC_MAX_NAME + 32, "%u %s %.*s\n", TASKFORGE_IPC_VERSION, name, (int)len, arg ? arg : "");
    int rc = write_all(fd, request, (size_t)n); free(request); if (rc) { close(fd); return -1; }
    struct timespec deadline;
    if (deadline_after(timeout_ms, &deadline) != 0) { close(fd); return -1; }
    char header[128];
    rc = read_line_until(fd, header, sizeof(header), &deadline);
    if (rc) { close(fd); return rc; }
    unsigned version, ok;
    int err, consumed = 0;
    size_t length;
    if (sscanf(header, "%u %u %d %zu %n", &version, &ok, &err, &length, &consumed) != 4 ||
        consumed <= 0 || header[consumed] != '\0' || version != TASKFORGE_IPC_VERSION ||
        ok > 1 || length >= cap || length > TASKFORGE_IPC_MAX_PAYLOAD) {
        close(fd);
        return -1;
    }
    rc = read_exact_until(fd, result, length, &deadline);
    if (rc == 0) {
        char delimiter = '\0';
        rc = read_exact_until(fd, &delimiter, 1, &deadline);
        if (rc == 0 && delimiter != '\n') rc = -1;
    }
    if (rc == 0) {
        result[length] = '\0';
        if (task_error) *task_error = err;
    }
    close(fd);
    return rc ? rc : (ok ? 0 : (err ? err : TASKFORGE_ERR_FAILED));
}
#else
int taskforge_ipc_server_run(const char* p, const taskforge_ipc_handler_t* h, size_t n, uint32_t m) { (void)p;(void)h;(void)n;(void)m; return TASKFORGE_ERR_INVALID; }
int taskforge_ipc_server_stop(const char* p) { (void)p; return TASKFORGE_ERR_INVALID; }
int taskforge_ipc_client_call(const char* p,const char* n,const char* a,size_t l,char*r,size_t c,int*e,uint32_t t){(void)p;(void)n;(void)a;(void)l;(void)r;(void)c;(void)e;(void)t;return TASKFORGE_ERR_INVALID;}
#endif
