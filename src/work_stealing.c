#include "taskforge/work_stealing.h"
#include "taskforge/future.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

bool ws_deque_init(ws_deque_t* deque, size_t capacity) {
    if (!deque) return false;
    if (capacity == 0) capacity = 512;
    if (capacity > SIZE_MAX / sizeof(taskforge_task_t)) return false;

    deque->buffer = (taskforge_task_t*)malloc(sizeof(taskforge_task_t) * capacity);
    if (!deque->buffer) return false;

    if (pthread_mutex_init(&deque->mutex, NULL) != 0) {
        free(deque->buffer);
        return false;
    }

    deque->capacity = capacity;
    deque->top = 0;
    deque->bottom = 0;
    deque->count = 0;
    return true;
}

void ws_deque_destroy(ws_deque_t* deque) {
    if (!deque) return;

    while (1) {
        taskforge_task_t task;
        pthread_mutex_lock(&deque->mutex);
        if (!deque->buffer || deque->count == 0) {
            pthread_mutex_unlock(&deque->mutex);
            break;
        }
        task = deque->buffer[deque->top];
        deque->top = (deque->top + 1) % deque->capacity;
        deque->count--;
        if (deque->count == 0) {
            deque->top = 0;
            deque->bottom = 0;
        }
        pthread_mutex_unlock(&deque->mutex);

        if (task.cleanup) task.cleanup(task.arg);
        if (task.future) future_fail(task.future, TASKFORGE_ERR_SHUTDOWN);
    }

    pthread_mutex_lock(&deque->mutex);
    free(deque->buffer);
    deque->buffer = NULL;
    pthread_mutex_unlock(&deque->mutex);
    pthread_mutex_destroy(&deque->mutex);
}

bool ws_deque_push_bottom(ws_deque_t* deque, const taskforge_task_t* task) {
    if (!deque || !task) return false;

    pthread_mutex_lock(&deque->mutex);
    if (deque->count >= deque->capacity) {
        pthread_mutex_unlock(&deque->mutex);
        return false; /* Deque full */
    }

    deque->buffer[deque->bottom] = *task;
    deque->bottom = (deque->bottom + 1) % deque->capacity;
    deque->count++;

    pthread_mutex_unlock(&deque->mutex);
    return true;
}

bool ws_deque_pop_bottom(ws_deque_t* deque, taskforge_task_t* out_task) {
    if (!deque || !out_task) return false;

    pthread_mutex_lock(&deque->mutex);
    if (deque->count == 0) {
        deque->top = 0;
        deque->bottom = 0;
        pthread_mutex_unlock(&deque->mutex);
        return false;
    }

    /* Pop from bottom: move bottom back by 1 */
    deque->bottom = (deque->bottom + deque->capacity - 1) % deque->capacity;
    *out_task = deque->buffer[deque->bottom];
    deque->count--;
    if (deque->count == 0) {
        deque->top = 0;
        deque->bottom = 0;
    }

    pthread_mutex_unlock(&deque->mutex);
    return true;
}

bool ws_deque_steal_top(ws_deque_t* deque, taskforge_task_t* out_task) {
    if (!deque || !out_task) return false;

    /* Stealers use trylock to avoid blocking heavily on busy workers */
    if (pthread_mutex_trylock(&deque->mutex) != 0) {
        return false;
    }

    if (deque->count == 0) {
        deque->top = 0;
        deque->bottom = 0;
        pthread_mutex_unlock(&deque->mutex);
        return false;
    }

    /* Steal from top (FIFO) */
    *out_task = deque->buffer[deque->top];
    deque->top = (deque->top + 1) % deque->capacity;
    deque->count--;
    if (deque->count == 0) {
        deque->top = 0;
        deque->bottom = 0;
    }

    pthread_mutex_unlock(&deque->mutex);
    return true;
}

size_t ws_deque_size(ws_deque_t* deque) {
    if (!deque) return 0;
    pthread_mutex_lock(&deque->mutex);
    size_t c = deque->count;
    pthread_mutex_unlock(&deque->mutex);
    return c;
}
