#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
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

static int fail_without_error_handler(const char* arg, size_t length, char* result,
                                            size_t capacity, int* error_code, void* context) {
    (void)arg; (void)length; (void)result; (void)capacity; (void)error_code; (void)context;
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
    char response[128];
    size_t used = 0;
    while (used + 1 < sizeof(response)) {
        ssize_t received = recv(fd, response + used, 1, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;
        if (response[used++] == '\n') break;
    }
    response[used] = '\0';
    close(fd);
    return used > 0 && strncmp(response, "1 0 ", 4) == 0 ? 0 : -1;
}

static void test_reject_ambiguous_text_arguments(const char* path) {
    char result[32];
    int error = 0;
    static const char newline_arg[] = "bad\narg";
    static const char carriage_arg[] = "bad\rarg";
    static const char nul_arg[] = {'a', '\0', 'b'};
    assert(taskforge_ipc_client_call(path, "echo", newline_arg, sizeof(newline_arg) - 1,
                                     result, sizeof(result), &error, 100) == -1);
    assert(taskforge_ipc_client_call(path, "echo", carriage_arg, sizeof(carriage_arg) - 1,
                                     result, sizeof(result), &error, 100) == -1);
    assert(taskforge_ipc_client_call(path, "echo", nul_arg, sizeof(nul_arg),
                                     result, sizeof(result), &error, 100) == -1);
    puts("  [PASS] IPC rejects NUL/CR/LF request arguments before connecting.");
}

static void test_path_safety(const char* path, const taskforge_ipc_handler_t* handlers,
                             size_t count) {
    static const char marker[] = "keep this file";
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, marker, sizeof(marker)) == (ssize_t)sizeof(marker));
    assert(close(fd) == 0);

    /* Starting/stopping IPC must never unlink a non-socket path. */
    assert(taskforge_ipc_server_run(path, handlers, count, 1) == -1);
    assert(taskforge_ipc_server_stop(path) == -1);
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    char content[sizeof(marker)] = {0};
    assert(read(fd, content, sizeof(content)) == (ssize_t)sizeof(content));
    assert(close(fd) == 0);
    assert(memcmp(content, marker, sizeof(marker)) == 0);
    assert(unlink(path) == 0);
    puts("  [PASS] IPC server start/stop preserves existing non-socket files.");
}

static pid_t start_trickle_body_server(const char* path) {
    unlink(path);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int server = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server < 0) _exit(1);
        struct sockaddr_un address = {0};
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
        if (bind(server, (struct sockaddr*)&address, sizeof(address)) != 0 ||
            listen(server, 1) != 0) _exit(2);
        int client = accept(server, NULL, NULL);
        if (client < 0) _exit(3);
        char ch;
        do {
            ssize_t got = recv(client, &ch, 1, 0);
            if (got <= 0) _exit(4);
        } while (ch != '\n');

        static const char header[] = "1 1 0 4\n";
        if (send(client, header, sizeof(header) - 1, MSG_NOSIGNAL) !=
            (ssize_t)(sizeof(header) - 1)) _exit(5);
        static const char body[] = "okay\n";
        for (size_t i = 0; i < sizeof(body) - 1; i++) {
            if (send(client, &body[i], 1, MSG_NOSIGNAL) != 1) break;
            usleep(50000);
        }
        close(client);
        close(server);
        _exit(0);
    }
    for (int i = 0; i < 100 && access(path, F_OK) != 0; i++) usleep(10000);
    assert(access(path, F_OK) == 0);
    return pid;
}

static void test_total_response_deadline(const char* path) {
    pid_t server = start_trickle_body_server(path);
    char result[16];
    int error = -1;
    struct timespec start, end;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    int rc = taskforge_ipc_client_call(path, "echo", "x", 1, result, sizeof(result),
                                       &error, 100);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    int64_t elapsed_ms = (int64_t)(end.tv_sec - start.tv_sec) * 1000LL +
                         (int64_t)(end.tv_nsec - start.tv_nsec) / 1000000LL;
    assert(rc == -2);
    assert(elapsed_ms < 175);
    wait_server(path, server);
    puts("  [PASS] IPC response deadline is total, not reset by each body byte.");
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
        {"slow", slow_handler, NULL},
        {"silent-fail", fail_without_error_handler, NULL}
    };
    test_reject_ambiguous_text_arguments(path);
    test_path_safety(path, handlers, 5);
    test_total_response_deadline(path);
    pid_t server = start_server(path, handlers, 5, 1);
    char result[128]; int error = 0;
    /* Line-framed requests must reject payloads that cannot round-trip. */
    assert(taskforge_ipc_client_call(path, "echo", "line\nbreak", 10,
                                     result, sizeof(result), &error, 1000) == -1);
    const char binary_arg[] = {'a', '\0', 'b'};
    assert(taskforge_ipc_client_call(path, "echo", binary_arg, sizeof(binary_arg),
                                     result, sizeof(result), &error, 1000) == -1);
    assert(taskforge_ipc_client_call(path, "echo", "hello", 5, result, sizeof(result), &error, 1000) == 0);
    assert(strcmp(result, "hello") == 0 && error == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 5, 1);
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
    assert(raw_request(path, "999 echo x\n") == 0);
    wait_server(path, server);

    server = start_server(path, handlers, 5, 1);
    assert(taskforge_ipc_client_call(path, "slow", "x", 1, result, sizeof(result), &error, 10) != 0);
    wait_server(path, server);

    server = start_server(path, handlers, 5, 1);
    assert(taskforge_ipc_client_call(path, "fail", "x", 1, result, sizeof(result), &error, 1000) == 77);
    assert(error == 77);
    wait_server(path, server);

    server = start_server(path, handlers, 5, 1);
    assert(taskforge_ipc_client_call(path, "silent-fail", "", 0, result, sizeof(result), &error, 1000) == TASKFORGE_ERR_FAILED);
    assert(error == TASKFORGE_ERR_FAILED);
    wait_server(path, server);
    test_disconnect_recovery(path);
    printf("[PASS] test_ipc completed separate-process success, error, timeout, and disconnect recovery paths.\n");
    return 0;
}
