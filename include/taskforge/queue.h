#ifndef TASKFORGE_QUEUE_H
#define TASKFORGE_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "taskforge/taskforge.h"

typedef struct {
    uint64_t task_id;
    taskforge_task_fn fn;
    taskforge_task_status_fn status_fn;
    void* arg;
    taskforge_future_t* future;
    taskforge_task_priority_t prio;
    taskforge_task_cleanup_fn cleanup;
} taskforge_task_t;

/* Single ring buffer */
typedef struct {
    taskforge_task_t* buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
} ring_buffer_t;

/* Thread-safe bounded task queue (with optional multi-priority).
 * Producer wake-up order is scheduler-dependent; no strict FIFO fairness is promised. */
typedef struct taskforge_queue {
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    bool enable_priority;
    size_t total_capacity;
    size_t total_count;

    /* Single ring-buffer or multi-level priority ring-buffers */
    ring_buffer_t ring[TASKFORGE_PRIO_COUNT];

    /* Starvation avoidance counter */
    size_t high_prio_streak;

    bool draining;    /* Set during graceful shutdown: reject new tasks, drain existing */
    bool shutdown;    /* Immediate termination flag */
} taskforge_queue_t;

/* Queue API */
taskforge_queue_t* queue_create(size_t capacity, bool enable_priority);
void queue_destroy(taskforge_queue_t* queue);

/* Push task: blocking, try-push, or timeout */
taskforge_status_t queue_push(taskforge_queue_t* queue, const taskforge_task_t* task);
taskforge_status_t queue_try_push(taskforge_queue_t* queue, const taskforge_task_t* task);
taskforge_status_t queue_push_timeout(taskforge_queue_t* queue, const taskforge_task_t* task, uint32_t timeout_ms);

/* Pop task: blocking until item available or queue stopped */
bool queue_pop(taskforge_queue_t* queue, taskforge_task_t* out_task);
bool queue_try_pop(taskforge_queue_t* queue, taskforge_task_t* out_task);

/* Queue management */
void queue_signal_shutdown(taskforge_queue_t* queue, bool graceful);
size_t queue_size(taskforge_queue_t* queue);
bool queue_is_empty(taskforge_queue_t* queue);

#endif /* TASKFORGE_QUEUE_H */
