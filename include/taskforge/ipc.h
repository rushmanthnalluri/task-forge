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
 * pointers or process-local addresses from a client. The requested socket path
 * must not already exist; stale paths must be removed explicitly by their owner. */
int taskforge_ipc_server_run(const char* socket_path,
                             const taskforge_ipc_handler_t* handlers,
                             size_t handler_count,
                             uint32_t max_requests);
/* Unlinks an existing socket pathname only. This does not terminate a server
 * process that is already listening; call only when the caller owns the path. */
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
