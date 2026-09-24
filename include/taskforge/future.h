#ifndef TASKFORGE_FUTURE_H
#define TASKFORGE_FUTURE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "taskforge/taskforge.h"

struct taskforge_future {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    atomic_int      ref_count;         /* Incremented when referenced by pool/caller */
    taskforge_future_state_t state;    /* PENDING, RUNNING, COMPLETED, CANCELLED, FAILED */
    void*           result;
    int             error_code;
    uint64_t        task_id;
    struct timespec submit_time;
    struct timespec start_time;
    struct timespec end_time;
};

/* Internal future helper functions */
taskforge_future_t* future_create(uint64_t task_id);
void future_retain(taskforge_future_t* future);
void future_release(taskforge_future_t* future);
bool future_mark_running(taskforge_future_t* future);
void future_complete(taskforge_future_t* future, void* result);
void future_fail(taskforge_future_t* future, int error_code);

#endif /* TASKFORGE_FUTURE_H */
