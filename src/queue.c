#include "taskforge/queue.h"
#include "taskforge/future.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define STARVATION_THRESHOLD 5

static inline void ring_push_internal(ring_buffer_t* ring, const taskforge_task_t* task);
static inline void ring_pop_internal(ring_buffer_t* ring, taskforge_task_t* out_task);

taskforge_queue_t* queue_create(size_t capacity, bool enable_priority) {
    if (capacity == 0) capacity = 1024;
    if (capacity > SIZE_MAX / sizeof(taskforge_task_t)) return NULL;

    taskforge_queue_t* q = (taskforge_queue_t*)calloc(1, sizeof(taskforge_queue_t));
    if (!q) return NULL;

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        free(q);
        return NULL;
    }
    pthread_condattr_t cond_attr;
    bool cond_attr_ready = (pthread_condattr_init(&cond_attr) == 0);
    if (!cond_attr_ready ||
        pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&q->not_empty, &cond_attr) != 0) {
        if (cond_attr_ready) pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&q->mutex);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_full, &cond_attr) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&q->mutex);
        free(q);
        return NULL;
    }
    pthread_condattr_destroy(&cond_attr);

    q->enable_priority = enable_priority;
    q->total_capacity = capacity;
    q->total_count = 0;
    q->high_prio_streak = 0;
    q->producer_next_ticket = 0;
    q->producer_turn = 0;
    q->draining = false;
    q->shutdown = false;

    size_t num_rings = enable_priority ? TASKFORGE_PRIO_COUNT : 1;
    for (size_t i = 0; i < num_rings; i++) {
        q->ring[i].capacity = capacity;
        q->ring[i].buffer = (taskforge_task_t*)malloc(sizeof(taskforge_task_t) * capacity);
        if (!q->ring[i].buffer) {
            queue_destroy(q);
            return NULL;
        }
        q->ring[i].head = 0;
        q->ring[i].tail = 0;
        q->ring[i].count = 0;
    }

    return q;
}

void queue_destroy(taskforge_queue_t* q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->shutdown = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    size_t num_rings = q->enable_priority ? TASKFORGE_PRIO_COUNT : 1;
    for (size_t i = 0; i < num_rings; i++) {
        if (q->ring[i].buffer) {
            while (1) {
                taskforge_task_t task;
                pthread_mutex_lock(&q->mutex);
                if (q->ring[i].count == 0) {
                    pthread_mutex_unlock(&q->mutex);
                    break;
                }
                ring_pop_internal(&q->ring[i], &task);
                if (q->total_count > 0) q->total_count--;
                pthread_mutex_unlock(&q->mutex);

                if (task.cleanup) task.cleanup(task.arg);
                if (task.future) future_fail(task.future, TASKFORGE_ERR_SHUTDOWN);
            }
            free(q->ring[i].buffer);
            q->ring[i].buffer = NULL;
        }
    }

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->canceled_tickets);
    free(q);
}

static inline size_t get_ring_index(taskforge_queue_t* q, taskforge_task_priority_t prio) {
    if (!q->enable_priority) return 0;
    if (prio > TASKFORGE_PRIO_HIGH) return TASKFORGE_PRIO_NORMAL;
    return (size_t)prio;
}

static inline void ring_push_internal(ring_buffer_t* ring, const taskforge_task_t* task) {
    ring->buffer[ring->tail] = *task;
    ring->tail = (ring->tail + 1) % ring->capacity;
    ring->count++;
}

static inline void ring_pop_internal(ring_buffer_t* ring, taskforge_task_t* out_task) {
    *out_task = ring->buffer[ring->head];
    ring->head = (ring->head + 1) % ring->capacity;
    ring->count--;
}

taskforge_status_t queue_push(taskforge_queue_t* q, const taskforge_task_t* task) {
    if (!q || !task) return TASKFORGE_ERR_INVALID;

    pthread_mutex_lock(&q->mutex);
    while (q->total_count >= q->total_capacity && !q->shutdown && !q->draining) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    if (q->shutdown || q->draining) {
        pthread_mutex_unlock(&q->mutex);
        return TASKFORGE_ERR_SHUTDOWN;
    }

    size_t ring_idx = get_ring_index(q, task->prio);
    ring_push_internal(&q->ring[ring_idx], task);
    q->total_count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return TASKFORGE_OK;
}

taskforge_status_t queue_try_push(taskforge_queue_t* q, const taskforge_task_t* task) {
    if (!q || !task) return TASKFORGE_ERR_INVALID;

    pthread_mutex_lock(&q->mutex);
    if (q->shutdown || q->draining) {
        pthread_mutex_unlock(&q->mutex);
        return TASKFORGE_ERR_SHUTDOWN;
    }
    if (q->total_count >= q->total_capacity) {
        pthread_mutex_unlock(&q->mutex);
        return TASKFORGE_ERR_FULL;
    }

    size_t ring_idx = get_ring_index(q, task->prio);
    ring_push_internal(&q->ring[ring_idx], task);
    q->total_count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return TASKFORGE_OK;
}

taskforge_status_t queue_push_timeout(taskforge_queue_t* q, const taskforge_task_t* task, uint32_t timeout_ms) {
    if (!q || !task) return TASKFORGE_ERR_INVALID;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->mutex);
    int rc = 0;
    while (q->total_count >= q->total_capacity && !q->shutdown && !q->draining && rc == 0) {
        rc = pthread_cond_timedwait(&q->not_full, &q->mutex, &ts);
    }
    if (rc != 0 && rc != ETIMEDOUT) {
        pthread_mutex_unlock(&q->mutex);
        return TASKFORGE_ERR_FAILED;
    }
    if (q->shutdown || q->draining) {
        pthread_mutex_unlock(&q->mutex);
        return TASKFORGE_ERR_SHUTDOWN;
    }
    if (q->total_count >= q->total_capacity) {
        pthread_mutex_unlock(&q->mutex);
        return rc == ETIMEDOUT ? TASKFORGE_ERR_TIMEOUT : TASKFORGE_ERR_FULL;
    }

    size_t ring_idx = get_ring_index(q, task->prio);
    ring_push_internal(&q->ring[ring_idx], task);
    q->total_count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return TASKFORGE_OK;
}

static size_t select_pop_ring(taskforge_queue_t* q) {
    if (!q->enable_priority) return 0;

    /*
     * Starvation avoidance: after a bounded HIGH streak, service the
     * lowest-priority tier that is waiting. Prefer LOW when present so a
     * continuously populated NORMAL tier cannot starve LOW indefinitely.
     */
    if (q->high_prio_streak >= STARVATION_THRESHOLD) {
        if (q->ring[TASKFORGE_PRIO_LOW].count > 0) {
            q->high_prio_streak = 0;
            return TASKFORGE_PRIO_LOW;
        }
        if (q->ring[TASKFORGE_PRIO_NORMAL].count > 0) {
            q->high_prio_streak = 0;
            return TASKFORGE_PRIO_NORMAL;
        }
    }

    /* Standard priority check: HIGH -> NORMAL -> LOW */
    if (q->ring[TASKFORGE_PRIO_HIGH].count > 0) {
        q->high_prio_streak++;
        return TASKFORGE_PRIO_HIGH;
    }
    q->high_prio_streak = 0;
    if (q->ring[TASKFORGE_PRIO_NORMAL].count > 0) {
        return TASKFORGE_PRIO_NORMAL;
    }
    return TASKFORGE_PRIO_LOW;
}

bool queue_pop(taskforge_queue_t* q, taskforge_task_t* out_task) {
    if (!q || !out_task) return false;

    pthread_mutex_lock(&q->mutex);

    while (q->total_count == 0 && !q->shutdown && !(q->draining && q->total_count == 0)) {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += 50000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&q->not_empty, &q->mutex, &deadline) == ETIMEDOUT)
            break;
    }

    if (q->total_count == 0) {
        /* Queue is shutting down and completely drained */
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    size_t idx = select_pop_ring(q);
    ring_pop_internal(&q->ring[idx], out_task);
    q->total_count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return true;
}

bool queue_try_pop(taskforge_queue_t* q, taskforge_task_t* out_task) {
    if (!q || !out_task) return false;

    pthread_mutex_lock(&q->mutex);
    if (q->total_count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    size_t idx = select_pop_ring(q);
    ring_pop_internal(&q->ring[idx], out_task);
    q->total_count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return true;
}

void queue_signal_shutdown(taskforge_queue_t* q, bool graceful) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    if (graceful) {
        q->draining = true;
    } else {
        q->shutdown = true;
        q->high_prio_streak = 0;
    }
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    if (!graceful) {
        /* Detach pending tasks under the queue lock, then invoke user cleanup
         * and future completion outside the lock to prevent re-entrant deadlocks. */
        while (1) {
            taskforge_task_t task;
            bool found = false;
            pthread_mutex_lock(&q->mutex);
            size_t num_rings = q->enable_priority ? TASKFORGE_PRIO_COUNT : 1;
            for (size_t i = 0; i < num_rings; i++) {
                if (q->ring[i].count > 0) {
                    ring_pop_internal(&q->ring[i], &task);
                    q->total_count--;
                    found = true;
                    break;
                }
            }
            pthread_mutex_unlock(&q->mutex);
            if (!found) break;

            if (task.cleanup) task.cleanup(task.arg);
            if (task.future) future_fail(task.future, TASKFORGE_ERR_SHUTDOWN);
        }
    }
}

size_t queue_size(taskforge_queue_t* q) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    size_t count = q->total_count;
    pthread_mutex_unlock(&q->mutex);
    return count;
}

bool queue_is_empty(taskforge_queue_t* q) {
    return queue_size(q) == 0;
}
