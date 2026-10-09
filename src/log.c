#include "taskforge/log.h"
#include <stdlib.h>
#include <time.h>

taskforge_logger_t* taskforge_logger_create(const char* filepath) {
    if (!filepath) return NULL;

    taskforge_logger_t* logger = (taskforge_logger_t*)calloc(1, sizeof(taskforge_logger_t));
    if (!logger) return NULL;

    logger->file = fopen(filepath, "a");
    if (!logger->file) {
        free(logger);
        return NULL;
    }

    if (pthread_mutex_init(&logger->mutex, NULL) != 0) {
        fclose(logger->file);
        free(logger);
        return NULL;
    }

    logger->enabled = true;
    fprintf(logger->file, "# TaskForge Log Started\n");
    fflush(logger->file);
    return logger;
}

void taskforge_logger_destroy(taskforge_logger_t* logger) {
    if (!logger) return;

    pthread_mutex_lock(&logger->mutex);
    if (logger->file) {
        fprintf(logger->file, "# TaskForge Log Closed\n");
        fflush(logger->file);
        fclose(logger->file);
        logger->file = NULL;
    }
    pthread_mutex_unlock(&logger->mutex);
    pthread_mutex_destroy(&logger->mutex);
    free(logger);
}

void taskforge_log_task(taskforge_logger_t* logger,
                        uint64_t task_id,
                        size_t worker_id,
                        taskforge_future_state_t state,
                        double duration_ms,
                        void* result,
                        int error_code) {
    if (!logger || !logger->enabled || !logger->file) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    gmtime_r(&ts.tv_sec, &tm_buf);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    const char* state_str = "UNKNOWN";
    switch (state) {
        case TASKFORGE_FUTURE_PENDING:   state_str = "PENDING"; break;
        case TASKFORGE_FUTURE_RUNNING:   state_str = "RUNNING"; break;
        case TASKFORGE_FUTURE_COMPLETED: state_str = "COMPLETED"; break;
        case TASKFORGE_FUTURE_CANCELLED: state_str = "CANCELLED"; break;
        case TASKFORGE_FUTURE_FAILED:    state_str = "FAILED"; break;
    }

    pthread_mutex_lock(&logger->mutex);
    fprintf(logger->file, "[%s.%03ld] [TASK %lu] [WORKER %zu] [STATE: %s] [DUR: %.3f ms] [RESULT: %p] [ERR: %d]\n",
            time_str, ts.tv_nsec / 1000000L, (unsigned long)task_id, worker_id, state_str, duration_ms, result, error_code);
    fflush(logger->file);
    pthread_mutex_unlock(&logger->mutex);
}
