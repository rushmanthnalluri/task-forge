#include "taskforge/future.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>

taskforge_future_t* future_create(uint64_t task_id) {
    taskforge_future_t* f = (taskforge_future_t*)calloc(1, sizeof(taskforge_future_t));
    if (!f) return NULL;

    if (pthread_mutex_init(&f->mutex, NULL) != 0) {
        free(f);
        return NULL;
    }
    if (pthread_cond_init(&f->cond, NULL) != 0) {
        pthread_mutex_destroy(&f->mutex);
        free(f);
        return NULL;
    }

    atomic_init(&f->ref_count, 2); /* 1 for caller, 1 for pool/worker */
    f->state = TASKFORGE_FUTURE_PENDING;
    f->result = NULL;
    f->error_code = 0;
    f->task_id = task_id;
    clock_gettime(CLOCK_MONOTONIC, &f->submit_time);

    return f;
}

void future_retain(taskforge_future_t* future) {
    if (!future) return;
    atomic_fetch_add(&future->ref_count, 1);
}

void future_release(taskforge_future_t* future) {
    if (!future) return;
    if (atomic_fetch_sub(&future->ref_count, 1) == 1) {
        pthread_mutex_destroy(&future->mutex);
        pthread_cond_destroy(&future->cond);
        free(future);
    }
}

bool future_mark_running(taskforge_future_t* future) {
    if (!future) return false;
    pthread_mutex_lock(&future->mutex);
    if (future->state != TASKFORGE_FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return false;
    }
    future->state = TASKFORGE_FUTURE_RUNNING;
    clock_gettime(CLOCK_MONOTONIC, &future->start_time);
    pthread_mutex_unlock(&future->mutex);
    return true;
}

void future_complete(taskforge_future_t* future, void* result) {
    if (!future) return;
    pthread_mutex_lock(&future->mutex);
    if (future->state != TASKFORGE_FUTURE_CANCELLED) {
        future->state = TASKFORGE_FUTURE_COMPLETED;
        future->result = result;
    }
    clock_gettime(CLOCK_MONOTONIC, &future->end_time);
    pthread_cond_broadcast(&future->cond);
    pthread_mutex_unlock(&future->mutex);
    future_release(future); /* Worker releases its reference */
}

void future_fail(taskforge_future_t* future, int error_code) {
    if (!future) return;
    pthread_mutex_lock(&future->mutex);
    if (future->state != TASKFORGE_FUTURE_CANCELLED) {
        future->state = TASKFORGE_FUTURE_FAILED;
        future->error_code = error_code;
    }
    clock_gettime(CLOCK_MONOTONIC, &future->end_time);
    pthread_cond_broadcast(&future->cond);
    pthread_mutex_unlock(&future->mutex);
    future_release(future); /* Worker releases its reference */
}

taskforge_status_t taskforge_future_wait(taskforge_future_t* future, void** out_result) {
    if (!future) return TASKFORGE_ERR_INVALID;

    pthread_mutex_lock(&future->mutex);
    while (future->state == TASKFORGE_FUTURE_PENDING || future->state == TASKFORGE_FUTURE_RUNNING) {
        pthread_cond_wait(&future->cond, &future->mutex);
    }

    taskforge_status_t status;
    if (future->state == TASKFORGE_FUTURE_COMPLETED) {
        if (out_result) *out_result = future->result;
        status = TASKFORGE_OK;
    } else if (future->state == TASKFORGE_FUTURE_CANCELLED) {
        status = TASKFORGE_ERR_CANCELLED;
    } else {
        status = TASKFORGE_ERR_FAILED;
    }

    pthread_mutex_unlock(&future->mutex);
    return status;
}

taskforge_status_t taskforge_future_wait_timeout(taskforge_future_t* future, uint32_t timeout_ms, void** out_result) {
    if (!future) return TASKFORGE_ERR_INVALID;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&future->mutex);
    int rc = 0;
    while ((future->state == TASKFORGE_FUTURE_PENDING || future->state == TASKFORGE_FUTURE_RUNNING) && rc == 0) {
        rc = pthread_cond_timedwait(&future->cond, &future->mutex, &ts);
    }

    taskforge_status_t status;
    if (future->state == TASKFORGE_FUTURE_COMPLETED) {
        if (out_result) *out_result = future->result;
        status = TASKFORGE_OK;
    } else if (future->state == TASKFORGE_FUTURE_CANCELLED) {
        status = TASKFORGE_ERR_CANCELLED;
    } else if (future->state == TASKFORGE_FUTURE_FAILED) {
        status = TASKFORGE_ERR_FAILED;
    } else if (rc == ETIMEDOUT) {
        status = TASKFORGE_ERR_TIMEOUT;
    } else {
        status = TASKFORGE_ERR_FAILED;
    }

    pthread_mutex_unlock(&future->mutex);
    return status;
}

int taskforge_future_get_error(taskforge_future_t* future) {
    if (!future) return TASKFORGE_ERR_INVALID;
    pthread_mutex_lock(&future->mutex);
    int error_code = future->error_code;
    pthread_mutex_unlock(&future->mutex);
    return error_code;
}

taskforge_future_state_t taskforge_future_get_state(taskforge_future_t* future) {
    if (!future) return TASKFORGE_FUTURE_FAILED;
    pthread_mutex_lock(&future->mutex);
    taskforge_future_state_t state = future->state;
    pthread_mutex_unlock(&future->mutex);
    return state;
}

bool taskforge_future_cancel(taskforge_future_t* future) {
    if (!future) return false;
    pthread_mutex_lock(&future->mutex);
    if (future->state == TASKFORGE_FUTURE_PENDING) {
        future->state = TASKFORGE_FUTURE_CANCELLED;
        pthread_cond_broadcast(&future->cond);
        pthread_mutex_unlock(&future->mutex);
        return true;
    }
    pthread_mutex_unlock(&future->mutex);
    return false;
}

void taskforge_future_release(taskforge_future_t* future) {
    future_release(future);
}
