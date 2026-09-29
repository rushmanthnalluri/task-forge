#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
#include "taskforge/taskforge.h"

static atomic_int g_executed_tasks = 0;

static void* counting_task(void* arg) {
    (void)arg;
    usleep(1000); /* 1ms work */
    atomic_fetch_add(&g_executed_tasks, 1);
    return NULL;
}

static void* immediate_blocker(void* arg) {
    (void)arg;
    usleep(200000);
    atomic_fetch_add(&g_executed_tasks, 1);
    return NULL;
}

int main(void) {
    printf("[TEST] Running test_shutdown...\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 2;
    cfg.queue_capacity = 128;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    int total_tasks = 50;
    for (int i = 0; i < total_tasks; i++) {
        taskforge_future_t* fut = taskforge_submit(pool, counting_task, NULL);
        assert(fut != NULL);
        taskforge_future_release(fut);
    }

    /* Initiate graceful shutdown */
    printf("  [Step] Initiating graceful shutdown with %d tasks in flight...\n", total_tasks);
    int rc = taskforge_pool_shutdown(pool, true);
    assert(rc == TASKFORGE_OK);

    /* Submit after shutdown must be rejected */
    taskforge_future_t* late_fut = taskforge_submit(pool, counting_task, NULL);
    assert(late_fut == NULL);
    printf("  [PASS] Submissions rejected during/after shutdown.\n");

    /* All 50 tasks must have executed */
    int completed = atomic_load(&g_executed_tasks);
    assert(completed == total_tasks);
    printf("  [PASS] Graceful drain completed all %d tasks before worker teardown.\n", completed);

    taskforge_pool_destroy(pool);
    printf("  [PASS] Graceful shutdown and pool destruction clean.\n");

    /* 2. Test immediate shutdown */
    printf("  [Step] Testing immediate shutdown...\n");
    taskforge_pool_t* pool_imm = taskforge_pool_create(&cfg);
    assert(pool_imm != NULL);

    for (int i = 0; i < 20; i++) {
        taskforge_future_t* fut = taskforge_submit(pool_imm, counting_task, NULL);
        if (fut) taskforge_future_release(fut);
    }

    rc = taskforge_pool_shutdown(pool_imm, false);
    assert(rc == TASKFORGE_OK);
    taskforge_pool_destroy(pool_imm);
    printf("  [PASS] Immediate shutdown terminated cleanly.\n");

    printf("[PASS] test_shutdown completed successfully!\n\n");
    return 0;
}
