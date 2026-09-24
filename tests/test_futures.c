#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

static void* slow_task(void* arg) {
    int ms = (int)(intptr_t)arg;
    usleep(ms * 1000);
    return (void*)((intptr_t)arg * 3);
}

int main(void) {
    printf("[TEST] Running test_futures...\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1; /* single worker to easily test queuing & cancellation */
    cfg.queue_capacity = 32;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    /* 1. Basic future result */
    taskforge_future_t* fut1 = taskforge_submit(pool, slow_task, (void*)(intptr_t)10);
    assert(fut1 != NULL);
    void* res1 = NULL;
    taskforge_status_t s1 = taskforge_future_wait(fut1, &res1);
    assert(s1 == TASKFORGE_OK);
    assert((intptr_t)res1 == 30);
    printf("  [PASS] Future basic wait returned correct value (30).\n");
    taskforge_future_release(fut1);

    /* 2. Timed future wait */
    taskforge_future_t* fut2 = taskforge_submit(pool, slow_task, (void*)(intptr_t)150);
    assert(fut2 != NULL);
    void* res2 = NULL;
    taskforge_status_t s2 = taskforge_future_wait_timeout(fut2, 30, &res2); /* 30ms timeout */
    assert(s2 == TASKFORGE_ERR_TIMEOUT);
    printf("  [PASS] Future wait_timeout correctly returned TASKFORGE_ERR_TIMEOUT (30ms < 150ms).\n");

    /* Now wait for completion */
    s2 = taskforge_future_wait(fut2, &res2);
    assert(s2 == TASKFORGE_OK);
    assert((intptr_t)res2 == 450);
    printf("  [PASS] Future wait succeeded after previous timeout with correct value (450).\n");
    taskforge_future_release(fut2);

    /* 3. Task cancellation of queued task */
    /* Saturate worker with a 150ms task */
    taskforge_future_t* blocker = taskforge_submit(pool, slow_task, (void*)(intptr_t)150);
    /* Submit task while worker is occupied */
    taskforge_future_t* to_cancel = taskforge_submit(pool, slow_task, (void*)(intptr_t)50);
    assert(to_cancel != NULL);

    /* Cancel before it starts */
    bool cancelled = taskforge_future_cancel(to_cancel);
    assert(cancelled == true);
    assert(taskforge_future_get_state(to_cancel) == TASKFORGE_FUTURE_CANCELLED);
    printf("  [PASS] taskforge_future_cancel successfully cancelled pending task.\n");

    void* res_cancel = NULL;
    taskforge_status_t sc = taskforge_future_wait(to_cancel, &res_cancel);
    assert(sc == TASKFORGE_ERR_CANCELLED);
    printf("  [PASS] Waiting on cancelled future returned TASKFORGE_ERR_CANCELLED.\n");

    taskforge_future_release(to_cancel);
    taskforge_future_wait(blocker, NULL);
    taskforge_future_release(blocker);

    taskforge_pool_destroy(pool);
    printf("[PASS] test_futures completed successfully!\n\n");
    return 0;
}
