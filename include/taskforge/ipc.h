#ifndef TASKFORGE_IPC_H
#define TASKFORGE_IPC_H

#include <stddef.h>
#include <stdint.h>
#include "taskforge/taskforge.h"

#define TASKFORGE_IPC_VERSION 1U
#define TASKFORGE_IPC_MAX_NAME 64U
#define TASKFORGE_IPC_MAX_PAYLOAD 4096U

typedef int (*taskforge_ipc_handler_fn)(const char* argument,
                                        size_t argument_length,
                                        char* result,
                                        size_t result_capacity,
                                        int* error_code,
                                        void* context);

typedef struct {
    const char* name;
    taskforge_ipc_handler_fn handler;
    void* context;
} taskforge_ipc_handler_t;

/* The server accepts only registered handlers. It never receives function
 * pointers or process-local addresses from a client. Arguments are text bytes:
 * embedded NUL, CR, and LF are rejected because requests use line framing.
 * Response results may contain newline characters. The requested socket path
 * must not already exist; stale paths must be removed explicitly by their owner.
 * The internal handler name "__taskforge_stop__" is reserved and cannot be
 * registered by applications. Any local process with write permission on the\n * socket can request stop, so callers must protect the socket path permissions. */
int taskforge_ipc_server_run(const char* socket_path,
                             const taskforge_ipc_handler_t* handlers,
                             size_t handler_count,
                             uint32_t max_requests);
/* Requests an orderly stop from the active server at socket_path and waits
 * for that server to close and remove its own socket inode. It never unlinks a
 * replacement path. Returns 0 only when stop is acknowledged and the original
 * socket pathname is gone or has been replaced; returns -1 when no compatible
 * server is listening or shutdown cannot be confirmed. */
int taskforge_ipc_server_stop(const char* socket_path);

int taskforge_ipc_client_call(const char* socket_path,
                              const char* handler_name,
                              const char* argument,
                              size_t argument_length,
                              char* result,
                              size_t result_capacity,
                              int* task_error,
                              uint32_t timeout_ms);

#endif
