#include "taskforge/taskforge.h"
#include <stdlib.h>

taskforge_status_t taskforge_map(taskforge_pool_t* pool,
                                 taskforge_task_fn map_fn,
                                 void** items,
                                 size_t count,
                                 void** results) {
    if (!pool || !map_fn || !items || count == 0) {
        return TASKFORGE_ERR_INVALID;
    }

    taskforge_future_t** futures = (taskforge_future_t**)malloc(sizeof(taskforge_future_t*) * count);
    if (!futures) return TASKFORGE_ERR_NOMEM;

    taskforge_status_t overall_status = TASKFORGE_OK;

    /* Submit all map items */
    for (size_t i = 0; i < count; i++) {
        futures[i] = taskforge_submit(pool, map_fn, items[i]);
        if (!futures[i]) {
            /* If submission fails (e.g. pool shutting down or out of memory) */
            overall_status = TASKFORGE_ERR_FAILED;
            for (size_t j = 0; j < i; j++) {
                taskforge_future_wait(futures[j], results ? &results[j] : NULL);
                taskforge_future_release(futures[j]);
            }
            free(futures);
            return overall_status;
        }
    }

    /* Wait for each item in order */
    for (size_t i = 0; i < count; i++) {
        void* res = NULL;
        taskforge_status_t s = taskforge_future_wait(futures[i], &res);
        if (results) results[i] = res;
        if (s != TASKFORGE_OK && overall_status == TASKFORGE_OK) {
            overall_status = s;
        }
        taskforge_future_release(futures[i]);
    }

    free(futures);
    return overall_status;
}
