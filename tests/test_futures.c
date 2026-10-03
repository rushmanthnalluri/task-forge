#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "taskforge/taskforge.h"
#include <stdatomic.h>

static atomic_int g_cleanup_calls = 0;
static atomic_bool g_blocker_started = false;
static void cleanup_arg(void* arg) {
    free(arg);
    atomic_fetch_add(&g_cleanup_calls, 1);
}

static void* slow_task(void* arg) {
    int ms = (int)(intptr_t)arg;
    usleep(ms * 1000);
    return (void*)((intptr_t)arg * 3);
}

static void* blocking_task(void* arg) {
    int ms = (int)(intptr_t)arg;
    atomic_store_explicit(&g_blocker_started, true, memory_order_release);
    usleep(ms * 1000);
    return (void*)((intptr_t)arg * 3);
}

static void wait_for_blocker(void) {
    for (int i = 0; i < 5000 && !atomic_load_explicit(&g_blocker_started, memory_order_acquire); i++) {
        usleep(1000);
    }
    assert(atomic_load_explicit(&g_blocker_started, memory_order_acquire));
}

int main(void) {
    printf("[TEST] Running test_futures...\n");

    taskforge_pool_config_t oversized_cfg;
    taskforge_default_config(&oversized_cfg);
    oversized_cfg.num_workers = SIZE_MAX;
    assert(taskforge_pool_create(&oversized_cfg) == NULL);
    oversized_cfg.num_workers = 1;
    oversized_cfg.queue_capacity = SIZE_MAX;
    assert(taskforge_pool_create(&oversized_cfg) == NULL);
    printf("  [PASS] Pool rejects worker/queue allocation-size overflow.\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1; /* single worker to easily test queuing & cancellation */
    cfg.queue_capacity = 32;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    /* Invalid priority values must be rejected rather than silently normalized. */
    assert(taskforge_submit_prio(pool, slow_task, NULL, (taskforge_task_priority_t)99) == NULL);
    assert(taskforge_try_submit(pool, slow_task, NULL, (taskforge_task_priority_t)99) == NULL);
    assert(taskforge_submit_timeout(pool, slow_task, NULL, (taskforge_task_priority_t)99, 1) == NULL);

    int* try_rejected_arg = malloc(sizeof(*try_rejected_arg));
    int* timeout_rejected_arg = malloc(sizeof(*timeout_rejected_arg));
    assert(try_rejected_arg != NULL && timeout_rejected_arg != NULL);
    *try_rejected_arg = 11;
    *timeout_rejected_arg = 12;
    assert(taskforge_try_submit_with_cleanup(pool, slow_task, try_rejected_arg,
                                              (taskforge_task_priority_t)99, cleanup_arg) == NULL);
    assert(taskforge_submit_timeout_with_cleanup(pool, slow_task, timeout_rejected_arg,
                                                  (taskforge_task_priority_t)99, 1,
                                                  cleanup_arg) == NULL);
    assert(atomic_load(&g_cleanup_calls) == 2);
    printf("  [PASS] Cleanup-aware try/timed submissions reclaim rejected arguments.\n");

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
    atomic_store_explicit(&g_blocker_started, false, memory_order_release);
    taskforge_future_t* blocker = taskforge_submit(pool, blocking_task, (void*)(intptr_t)150);
    assert(blocker != NULL);
    wait_for_blocker();
    /* Submit task only after the single worker is definitely occupied. */
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
    assert(atomic_load(&g_cleanup_calls) == 2);
    taskforge_future_wait(blocker, NULL);
    taskforge_future_release(blocker);

    /* 4. Cancellation cleanup callback must reclaim queued task arguments. */
    atomic_store_explicit(&g_blocker_started, false, memory_order_release);
    taskforge_future_t* cleanup_blocker = taskforge_submit(pool, blocking_task, (void*)(intptr_t)150);
    assert(cleanup_blocker != NULL);
    wait_for_blocker();
    int* owned_arg = malloc(sizeof(*owned_arg));
    assert(owned_arg != NULL);
    *owned_arg = 42;
    taskforge_future_t* cleanup_future = taskforge_submit_prio_with_cleanup(
        pool, slow_task, owned_arg, TASKFORGE_PRIO_NORMAL, cleanup_arg);
    assert(cleanup_future != NULL);
    assert(taskforge_future_cancel(cleanup_future));
    assert(taskforge_future_wait(cleanup_future, NULL) == TASKFORGE_ERR_CANCELLED);
    taskforge_future_release(cleanup_future);
    taskforge_future_wait(cleanup_blocker, NULL);
    taskforge_future_release(cleanup_blocker);
    assert(atomic_load(&g_cleanup_calls) == 3);
    printf("  [PASS] Cancelled queued task arguments were reclaimed by cleanup callback.\n");

    int* rejected_arg = malloc(sizeof(*rejected_arg));
    assert(rejected_arg != NULL);
    *rejected_arg = 7;
    assert(taskforge_submit_prio_with_cleanup(pool, slow_task, rejected_arg,
                                              (taskforge_task_priority_t)99, cleanup_arg) == NULL);
    assert(atomic_load(&g_cleanup_calls) == 4);
    printf("  [PASS] Rejected cleanup-aware submissions reclaim their arguments.\n");

    /* Force a bounded queue timeout and verify the timed cleanup contract. */
    taskforge_pool_config_t timeout_cfg = cfg;
    timeout_cfg.queue_capacity = 1;
    taskforge_pool_t* timeout_pool = taskforge_pool_create(&timeout_cfg);
    assert(timeout_pool != NULL);
    atomic_store_explicit(&g_blocker_started, false, memory_order_release);
    taskforge_future_t* timeout_blocker =
        taskforge_submit(timeout_pool, blocking_task, (void*)(intptr_t)150);
    assert(timeout_blocker != NULL);
    wait_for_blocker();
    taskforge_future_t* timeout_queued =
        taskforge_submit(timeout_pool, slow_task, (void*)(intptr_t)25);
    assert(timeout_queued != NULL);

    int* timed_out_arg = malloc(sizeof(*timed_out_arg));
    assert(timed_out_arg != NULL);
    *timed_out_arg = 99;
    assert(taskforge_submit_timeout_with_cleanup(
               timeout_pool, slow_task, timed_out_arg, TASKFORGE_PRIO_NORMAL,
               1, cleanup_arg) == NULL);
    assert(atomic_load(&g_cleanup_calls) == 5);

    assert(taskforge_future_wait(timeout_blocker, NULL) == TASKFORGE_OK);
    assert(taskforge_future_wait(timeout_queued, NULL) == TASKFORGE_OK);
    taskforge_future_release(timeout_blocker);
    taskforge_future_release(timeout_queued);
    taskforge_pool_shutdown(timeout_pool, true);
    taskforge_pool_destroy(timeout_pool);
    printf("  [PASS] Timed submission timeout reclaims its argument.\n");

    taskforge_pool_destroy(pool);
    printf("[PASS] test_futures completed successfully!\n\n");
    return 0;
}
