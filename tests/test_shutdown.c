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
    taskforge_pool_config_t imm_cfg = cfg;
    imm_cfg.num_workers = 1;
    taskforge_pool_t* pool_imm = taskforge_pool_create(&imm_cfg);
    assert(pool_imm != NULL);

    taskforge_future_t* blocker_imm = taskforge_submit(pool_imm, immediate_blocker, NULL);
    assert(blocker_imm != NULL);
    usleep(20000);

    taskforge_future_t* queued[20];
    for (int i = 0; i < 20; i++) {
        queued[i] = taskforge_submit(pool_imm, counting_task, NULL);
        assert(queued[i] != NULL);
    }

    rc = taskforge_pool_shutdown(pool_imm, false);
    assert(rc == TASKFORGE_OK);

    for (int i = 0; i < 20; i++) {
        assert(taskforge_future_get_state(queued[i]) == TASKFORGE_FUTURE_FAILED);
        assert(taskforge_future_wait(queued[i], NULL) == TASKFORGE_ERR_FAILED);
        assert(taskforge_future_get_error(queued[i]) == TASKFORGE_ERR_SHUTDOWN);
        taskforge_future_release(queued[i]);
    }
    assert(taskforge_future_wait(blocker_imm, NULL) == TASKFORGE_OK);
    taskforge_future_release(blocker_imm);

    taskforge_pool_destroy(pool_imm);
    printf("  [PASS] Immediate shutdown failed queued work and preserved the task already running.\n");

    printf("[PASS] test_shutdown completed successfully!\n\n");
    return 0;
}
