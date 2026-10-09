#ifndef TASKFORGE_LOG_H
#define TASKFORGE_LOG_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "taskforge/taskforge.h"

typedef struct {
    FILE* file;
    pthread_mutex_t mutex;
    bool enabled;
} taskforge_logger_t;

taskforge_logger_t* taskforge_logger_create(const char* filepath);
void taskforge_logger_destroy(taskforge_logger_t* logger);
void taskforge_log_task(taskforge_logger_t* logger,
                        uint64_t task_id,
                        size_t worker_id,
                        taskforge_future_state_t state,
                        double duration_ms,
                        void* result,
                        int error_code);

#endif /* TASKFORGE_LOG_H */
