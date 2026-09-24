#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

static void* compute_task(void* arg) {
    intptr_t v = (intptr_t)arg;
    for (volatile int i = 0; i < 2000; i++);
    return (void*)(v + 1);
}

int main(void) {
    printf("[TEST] Running test_work_stealing...\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 4;
    cfg.queue_capacity = 4096;
    cfg.enable_work_stealing = true;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    size_t count = 5000;
    taskforge_future_t** futs = (taskforge_future_t**)malloc(sizeof(taskforge_future_t*) * count);

    for (size_t i = 0; i < count; i++) {
        futs[i] = taskforge_submit(pool, compute_task, (void*)(intptr_t)i);
        assert(futs[i] != NULL);
    }

    for (size_t i = 0; i < count; i++) {
        void* res = NULL;
        taskforge_status_t s = taskforge_future_wait(futs[i], &res);
        assert(s == TASKFORGE_OK);
        assert((intptr_t)res == (intptr_t)(i + 1));
        taskforge_future_release(futs[i]);
    }
    free(futs);

    taskforge_pool_stats_t st = taskforge_pool_get_stats(pool);
    printf("  [Stats] Completed: %zu, Stolen: %zu across %zu workers.\n",
           st.completed_tasks, st.stolen_tasks, st.num_workers);

    assert(st.completed_tasks == count);
    printf("  [PASS] All %zu tasks processed accurately with work-stealing active.\n", count);

    taskforge_pool_destroy(pool);
    printf("[PASS] test_work_stealing completed successfully!\n\n");
    return 0;
}
