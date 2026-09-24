#ifndef TASKFORGE_WORK_STEALING_H
#define TASKFORGE_WORK_STEALING_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "taskforge/queue.h"

/* Per-worker work-stealing double-ended queue (deque) */
typedef struct {
    pthread_mutex_t mutex;
    taskforge_task_t* buffer;
    size_t capacity;
    size_t top;       /* Stealers take from top (FIFO) */
    size_t bottom;    /* Owner pushes & pops from bottom (LIFO) */
    size_t count;
} ws_deque_t;

/* Work-stealing deque operations */
bool ws_deque_init(ws_deque_t* deque, size_t capacity);
void ws_deque_destroy(ws_deque_t* deque);

/* Owner operations (invoked by the worker thread) */
bool ws_deque_push_bottom(ws_deque_t* deque, const taskforge_task_t* task);
bool ws_deque_pop_bottom(ws_deque_t* deque, taskforge_task_t* out_task);

/* Stealer operation (invoked by other idle worker threads) */
bool ws_deque_steal_top(ws_deque_t* deque, taskforge_task_t* out_task);

size_t ws_deque_size(ws_deque_t* deque);

#endif /* TASKFORGE_WORK_STEALING_H */
