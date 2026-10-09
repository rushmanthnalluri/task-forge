#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include "taskforge/ipc.h"

static int echo_handler(const char* arg, size_t length, char* result, size_t capacity,
                        int* error_code, void* context) {
    (void)context;
    if (length + 1 > capacity) { *error_code = TASKFORGE_ERR_NOMEM; return -1; }
    memcpy(result, arg, length);
    result[length] = '\0';
    *error_code = 0;
    return 0;
}

static int multiline_handler(const char* arg, size_t length, char* result, size_t capacity,
                             int* error_code, void* context) {
    (void)arg; (void)length; (void)context;
    static const char value[] = "first line\nsecond line";
    if (sizeof(value) > capacity) { *error_code = TASKFORGE_ERR_NOMEM; return -1; }
    memcpy(result, value, sizeof(value));
    *error_code = 0;
    return 0;
}

static int fail_handler(const char* arg, size_t length, char* result, size_t capacity,
                        int* error_code, void* context) {
    (void)arg; (void)length; (void)result; (void)capacity; (void)context;
    *error_code = 77;
    return -1;
}

static int slow_handler(const char* arg, size_t length, char* result, size_t capacity,
                        int* error_code, void* context) {
    (void)context;
    usleep(200000);
    return echo_handler(arg, length, result, capacity, error_code, NULL);
}

typedef struct {
    int entered;
    int release;
} handler_gate_t;

static int gated_handler(const char* arg, size_t length, char* result, size_t capacity,
                         int* error_code, void* context) {
    handler_gate_t* gate = context;
    char byte = 'x';
    assert(write(gate->entered, &byte, 1) == 1);
    assert(read(gate->release, &byte, 1) == 1);
    return echo_handler(arg, length, result, capacity, error_code, NULL);
}

static pid_t start_server(const char* path, const taskforge_ipc_handler_t* handlers,
                          size_t count, uint32_t requests) {
    unlink(path);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct sigaction before = {0}, after;
        before.sa_handler = SIG_DFL;
        sigemptyset(&before.sa_mask);
        assert(sigaction(SIGPIPE, &before, NULL) == 0);
        int rc = taskforge_ipc_server_run(path, handlers, count, requests);
        assert(sigaction(SIGPIPE, NULL, &after) == 0);
        assert(after.sa_handler == SIG_DFL);
        _exit(rc == 0 ? 0 : 1);
    }
    for (int i = 0; i < 100 && access(path, F_OK) != 0; i++) usleep(10000);
    assert(access(path, F_OK) == 0);
    return pid;
}

static void wait_server(const char* path, pid_t pid) {
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlink(path);
}

static int raw_request(const char* path, const char* request) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    assert(connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    size_t length = strlen(request);
    assert(send(fd, request, length, 0) == (ssize_t)length);
    shutdown(fd, SHUT_WR);
    char response[256];
    ssize_t received = recv(fd, response, sizeof(response) - 1, 0);
    close(fd);
    return received > 0 ? 0 : -1;
}

static void test_disconnect_recovery(const char* path) {
    int entered[2], release[2];
    assert(pipe(entered) == 0 && pipe(release) == 0);
    handler_gate_t gate = {entered[1], release[0]};
    const taskforge_ipc_handler_t handlers[] = {
        {"gated", gated_handler, &gate}, {"echo", echo_handler, NULL}
    };
    pid_t server = start_server(path, handlers, 2, 2);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    assert(connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    const char request[] = "1 gated disconnected\n";
    assert(send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) == sizeof(request) - 1);
    char byte;
    assert(read(entered[0], &byte, 1) == 1); /* callback has started */
    assert(shutdown(fd, SHUT_RDWR) == 0);
    close(fd);
    assert(write(release[1], "x", 1) == 1); /* now response delivery must fail safely */
    char result[32];
    int error = -1;
    assert(taskforge_ipc_client_call(path, "echo", "still alive", 11,
                                   result, sizeof(result), &error, 2000) == 0);
    assert(strcmp(result, "still alive") == 0 && error == 0);
    wait_server(path, server);
    close(entered[0]); close(entered[1]); close(release[0]); close(release[1]);
    puts("  [PASS] Disconnected response cannot kill server or alter SIGPIPE; next request succeeds.");
}

int main(void) {
    alarm(30); /* a deadlock fails instead of hanging the test runner */
    char path[100];
    snprintf(path, sizeof(path), "/tmp/taskforge-ipc-%ld.sock", (long)getpid());
    unlink(path);
    const taskforge_ipc_handler_t handlers[] = {
        {"echo", echo_handler, NULL},
        {"multiline", multiline_handler, NULL},
        {"fail", fail_handler, NULL},
        {"slow", slow_handler, NULL}
    };
    pid_t server = start_server(path, handlers, 4, 1);
    char result[128]; int error = 0;
    assert(taskforge_ipc_client_call(path, "echo", "hello", 5, result, sizeof(result), &error, 1000) == 0);
    assert(strcmp(result, "hello") == 0 && error == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 4, 1);
    assert(taskforge_ipc_client_call(path, "echo", "  hello", 7, result, sizeof(result), &error, 1000) == 0);
    assert(strcmp(result, "  hello") == 0 && error == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 4, 1);
    assert(taskforge_ipc_client_call(path, "multiline", "", 0, result, sizeof(result), &error, 1000) == 0);
    assert(strcmp(result, "first line\nsecond line") == 0 && error == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 4, 1);
    assert(taskforge_ipc_client_call(path, "unknown", "x", 1, result, sizeof(result), &error, 1000) != 0);
    wait_server(path, server);

    server = start_server(path, handlers, 3, 1);
    assert(raw_request(path, "999 bad\n") == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 3, 1);
    assert(taskforge_ipc_client_call(path, "slow", "x", 1, result, sizeof(result), &error, 10) != 0);
    wait_server(path, server);

    server = start_server(path, handlers, 3, 1);
    assert(taskforge_ipc_client_call(path, "fail", "x", 1, result, sizeof(result), &error, 1000) == 77);
    assert(error == 77);
    wait_server(path, server);
    test_disconnect_recovery(path);
    printf("[PASS] test_ipc completed separate-process success, error, timeout, and disconnect recovery paths.\n");
    return 0;
}
