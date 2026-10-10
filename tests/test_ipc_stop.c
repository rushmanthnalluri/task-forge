#include "taskforge/ipc.h"

#ifdef _WIN32
int main(void) {
    return 0;
}
#else
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    const char* path;
    int result;
} ipc_server_context_t;

static int ping_handler(const char* argument, size_t argument_length,
                        char* result, size_t result_capacity,
                        int* error_code, void* context) {
    (void)argument;
    (void)context;
    if (argument_length != 0 || result_capacity < sizeof("pong")) {
        if (error_code) *error_code = TASKFORGE_ERR_INVALID;
        return -1;
    }
    memcpy(result, "pong", sizeof("pong"));
    if (error_code) *error_code = 0;
    return 0;
}

static const taskforge_ipc_handler_t handlers[] = {
    {"ping", ping_handler, NULL}
};

static void* run_server(void* opaque) {
    ipc_server_context_t* context = (ipc_server_context_t*)opaque;
    context->result = taskforge_ipc_server_run(context->path, handlers, 1, 0);
    return NULL;
}

static void test_stop_active_server(void) {
    char template[] = "/tmp/taskforge-ipc-stop-XXXXXX";
    int tmp = mkstemp(template);
    assert(tmp >= 0);
    assert(close(tmp) == 0);
    assert(unlink(template) == 0);

    ipc_server_context_t context = {template, -999};
    pthread_t server_thread;
    assert(pthread_create(&server_thread, NULL, run_server, &context) == 0);

    char result[64] = {0};
    int task_error = 0;
    int call_rc = -1;
    for (int i = 0; i < 2000; i++) {
        call_rc = taskforge_ipc_client_call(template, "ping", "", 0,
                                            result, sizeof(result), &task_error, 100);
        if (call_rc == 0) break;
        usleep(1000);
    }
    assert(call_rc == 0);
    assert(strcmp(result, "pong") == 0);

    assert(taskforge_ipc_server_stop(template) == 0);
    assert(pthread_join(server_thread, NULL) == 0);
    assert(context.result == 0);

    struct stat st;
    errno = 0;
    assert(lstat(template, &st) != 0 && errno == ENOENT);
    printf("  [PASS] IPC server stop wakes accept and waits for owned socket cleanup.\n");
}

static void test_stop_preserves_non_socket_path(void) {
    char template[] = "/tmp/taskforge-ipc-stop-file-XXXXXX";
    int fd = mkstemp(template);
    assert(fd >= 0);
    assert(write(fd, "keep", 4) == 4);
    assert(close(fd) == 0);

    assert(taskforge_ipc_server_stop(template) == -1);
    struct stat st;
    assert(lstat(template, &st) == 0);
    assert(S_ISREG(st.st_mode));

    char bytes[5] = {0};
    fd = open(template, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, bytes, 4) == 4);
    assert(close(fd) == 0);
    assert(strcmp(bytes, "keep") == 0);
    assert(unlink(template) == 0);
    printf("  [PASS] IPC server stop never unlinks a regular file.\n");
}

static void test_stop_command_is_reserved(void) {
    char template[] = "/tmp/taskforge-ipc-reserved-XXXXXX";
    int fd = mkstemp(template);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(template) == 0);

    taskforge_ipc_handler_t reserved = {
        "__taskforge_stop__", ping_handler, NULL
    };
    assert(taskforge_ipc_server_run(template, &reserved, 1, 1) == -1);
    struct stat st;
    assert(lstat(template, &st) != 0 && errno == ENOENT);
    printf("  [PASS] Application handlers cannot shadow the internal stop command.\n");
}

int main(void) {
    alarm(15);
    printf("[TEST] Running IPC server stop regressions...\n");
    test_stop_active_server();
    test_stop_preserves_non_socket_path();
    test_stop_command_is_reserved();
    printf("[PASS] test_ipc_stop completed successfully.\n");
    return 0;
}
#endif
