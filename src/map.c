#include "taskforge/taskforge.h"
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "internal.h"

static taskforge_status_t map_impl(taskforge_pool_t* pool,
                                   taskforge_task_fn map_fn,
                                   void** items,
                                   size_t count,
                                   void** results,
                                   bool timed,
                                   uint32_t timeout_ms,
                                   taskforge_map_item_result_t* report) {
    if (!pool || !map_fn || (count > 0 && !items)) {
        return TASKFORGE_ERR_INVALID;
    }
    if (count == 0) return TASKFORGE_OK;
    if (report) {
        for (size_t i = 0; i < count; i++) {
            report[i].status = TASKFORGE_ERR_FAILED;
            report[i].task_error = TASKFORGE_ERR_FAILED;
            report[i].result = NULL;
        }
    }
    if (count > SIZE_MAX / sizeof(taskforge_future_t*)) {
        return TASKFORGE_ERR_NOMEM;
    }

    /*
     * Results are allowed to alias items (including items == results).  Keep
     * a stable snapshot of the input pointers before clearing result slots or
     * before any submitted task can write a result back into that storage.
     */
    void** input_copy = NULL;
    if (results) {
        input_copy = malloc(sizeof(*input_copy) * count);
        if (!input_copy) return TASKFORGE_ERR_NOMEM;
        for (size_t i = 0; i < count; i++) input_copy[i] = items[i];
    }
    void** map_items = input_copy ? input_copy : items;

    /*
     * A worker must not synchronously wait on work it just submitted when
     * it is the only worker capable of executing that work.  More generally,
     * keeping map reentrant makes the convenience API safe inside task
     * callbacks. Execute the mapping inline from a worker caller.
     */
    if (taskforge_pool_is_worker_thread(pool)) {
        if (results) {
            for (size_t i = 0; i < count; i++) results[i] = NULL;
        }
        taskforge_status_t status = TASKFORGE_OK;
        for (size_t i = 0; i < count; i++) {
            void* result = map_fn(map_items[i]);
            if (results) results[i] = result;
            if (report) {
                report[i].status = TASKFORGE_OK;
                report[i].task_error = 0;
                report[i].result = result;
            }
        }
        free(input_copy);
        return status;
    }

    taskforge_future_t** futures = malloc(sizeof(*futures) * count);
    if (!futures) {
        free(input_copy);
        return TASKFORGE_ERR_NOMEM;
    }

    taskforge_status_t overall_status = TASKFORGE_OK;
    if (results) {
        for (size_t i = 0; i < count; i++) results[i] = NULL;
    }

    /* Submit all map items */
    for (size_t i = 0; i < count; i++) {
        futures[i] = taskforge_submit(pool, map_fn, map_items[i]);
        if (!futures[i]) {
            /* If submission fails (e.g. pool shutting down or out of memory) */
            overall_status = TASKFORGE_ERR_FAILED;
            if (report) {
                report[i].status = TASKFORGE_ERR_FAILED;
                report[i].task_error = TASKFORGE_ERR_FAILED;
            }
            for (size_t j = 0; j < i; j++) {
                taskforge_status_t wait_status =
                    taskforge_future_wait(futures[j], results ? &results[j] : NULL);
                if (wait_status != TASKFORGE_OK && overall_status == TASKFORGE_OK) {
                    if (wait_status == TASKFORGE_ERR_FAILED) {
                        int error_code = taskforge_future_get_error(futures[j]);
                        overall_status = error_code != 0 ? error_code : wait_status;
                    } else {
                        overall_status = wait_status;
                    }
                }
                taskforge_future_release(futures[j]);
            }
            free(futures);
            free(input_copy);
            return overall_status;
        }
    }

    struct timespec deadline = {0};
    if (timed) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000U;
        deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    /* Wait for each item in order against one shared deadline. */
    for (size_t i = 0; i < count; i++) {
        void* res = NULL;
        uint32_t remaining = timeout_ms;
        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t ns = (int64_t)(deadline.tv_sec - now.tv_sec) * 1000000000LL +
                         (int64_t)deadline.tv_nsec - now.tv_nsec;
            remaining = ns <= 0 ? 0 : (uint32_t)((ns + 999999LL) / 1000000LL);
        }
        taskforge_status_t s = timed
            ? taskforge_future_wait_timeout(futures[i], remaining, &res)
            : taskforge_future_wait(futures[i], &res);
        if (results) results[i] = res;
        if (report) {
            report[i].status = s;
            report[i].task_error = taskforge_future_get_error(futures[i]);
            report[i].result = res;
        }
        if (s != TASKFORGE_OK && overall_status == TASKFORGE_OK) {
            overall_status = s;
            if (s == TASKFORGE_ERR_TIMEOUT) {
                /* If the timed-out item has not started, avoid running it after
                 * the caller has already received a timeout result. */
                (void)taskforge_future_cancel(futures[i]);
                for (size_t j = i + 1; j < count; j++) {
                    bool cancelled = taskforge_future_cancel(futures[j]);
                    void* item_result = NULL;
                    taskforge_status_t item_status = cancelled
                        ? TASKFORGE_ERR_CANCELLED
                        : taskforge_future_wait_timeout(futures[j], 0, &item_result);
                    if (report) {
                        report[j].status = item_status;
                        report[j].task_error = taskforge_future_get_error(futures[j]);
                        report[j].result = item_status == TASKFORGE_OK ? item_result : NULL;
                    }
                    taskforge_future_release(futures[j]);
                }
                taskforge_future_release(futures[i]);
                free(futures);
                free(input_copy);
                return overall_status;
            }
        }
        taskforge_future_release(futures[i]);
    }

    free(futures);
    free(input_copy);
    return overall_status;
}

taskforge_status_t taskforge_map(taskforge_pool_t* pool,
                                 taskforge_task_fn map_fn,
                                 void** items,
                                 size_t count,
                                 void** results) {
    return map_impl(pool, map_fn, items, count, results, false, 0, NULL);
}

taskforge_status_t taskforge_map_timeout(taskforge_pool_t* pool,
                                         taskforge_task_fn map_fn,
                                         void** items,
                                         size_t count,
                                         void** results,
                                         uint32_t timeout_ms) {
    return map_impl(pool, map_fn, items, count, results, true, timeout_ms, NULL);
}

taskforge_status_t taskforge_map_timeout_report(taskforge_pool_t* pool,
                                                taskforge_task_fn map_fn,
                                                void** items,
                                                size_t count,
                                                taskforge_map_item_result_t* report,
                                                uint32_t timeout_ms) {
    if (count > 0 && !report) return TASKFORGE_ERR_INVALID;
    return map_impl(pool, map_fn, items, count, NULL, true, timeout_ms, report);
}
