#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "taskforge/taskforge.h"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_exec_order[64];
static size_t g_order_count = 0;
static atomic_bool g_blocker_started = false;

static void* blocker_fn(void* arg) {
    (void)arg;
    atomic_store(&g_blocker_started, true);
    usleep(50000); /* 50ms hold after the worker is confirmed running */
    return NULL;
}

static void* ordered_task(void* arg) {
    int id = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_log_mutex);
    g_exec_order[g_order_count++] = id;
    pthread_mutex_unlock(&g_log_mutex);
    return NULL;
}

int main(void) {
    printf("[TEST] Running test_priorities...\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1; /* single worker to test strict queue order */
    cfg.queue_capacity = 64;
    cfg.enable_priority = true;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    /* Start a blocker and wait for positive confirmation that the worker is inside it. */
    taskforge_future_t* b = taskforge_submit(pool, blocker_fn, NULL);
    assert(b != NULL);
    for (int i = 0; i < 1000 && !atomic_load(&g_blocker_started); i++) {
        usleep(1000);
    }
    assert(atomic_load(&g_blocker_started));

    /* Submit 5 LOW priority tasks (IDs 101..105). */
    taskforge_future_t* low_futs[5];
    for (int i = 0; i < 5; i++) {
        low_futs[i] = taskforge_submit_prio(
            pool, ordered_task, (void*)(intptr_t)(101 + i), TASKFORGE_PRIO_LOW);
        assert(low_futs[i] != NULL);
    }

    /* Submit 5 NORMAL priority tasks (IDs 301..305). */
    taskforge_future_t* normal_futs[5];
    for (int i = 0; i < 5; i++) {
        normal_futs[i] = taskforge_submit_prio(
            pool, ordered_task, (void*)(intptr_t)(301 + i), TASKFORGE_PRIO_NORMAL);
        assert(normal_futs[i] != NULL);
    }

    /* Submit 5 HIGH priority tasks (IDs 201..205). */
    taskforge_future_t* high_futs[5];
    for (int i = 0; i < 5; i++) {
        high_futs[i] = taskforge_submit_prio(
            pool, ordered_task, (void*)(intptr_t)(201 + i), TASKFORGE_PRIO_HIGH);
        assert(high_futs[i] != NULL);
    }

    taskforge_future_wait(b, NULL);
    taskforge_future_release(b);

    for (int i = 0; i < 5; i++) {
        taskforge_future_wait(high_futs[i], NULL);
        taskforge_future_release(high_futs[i]);
    }
    for (int i = 0; i < 5; i++) {
        taskforge_future_wait(normal_futs[i], NULL);
        taskforge_future_release(normal_futs[i]);
    }
    for (int i = 0; i < 5; i++) {
        taskforge_future_wait(low_futs[i], NULL);
        taskforge_future_release(low_futs[i]);
    }

    pthread_mutex_lock(&g_log_mutex);
    printf("  Execution order of prioritized tasks: ");
    for (size_t i = 0; i < g_order_count; i++) {
        printf("%d ", g_exec_order[i]);
    }
    printf("\n");

    /* Verify HIGH tasks are prioritized, while LOW cannot starve behind NORMAL. */
    assert(g_order_count == 15);
    assert(g_exec_order[0] >= 201 && g_exec_order[0] <= 205);
    assert(g_exec_order[1] >= 201 && g_exec_order[1] <= 205);

    size_t first_low = g_order_count;
    for (size_t i = 0; i < g_order_count; i++) {
        if (g_exec_order[i] >= 101 && g_exec_order[i] <= 105) {
            first_low = i;
            break;
        }
    }
    assert(first_low <= 5);
    assert(first_low < g_order_count);
    pthread_mutex_unlock(&g_log_mutex);

    printf("  [PASS] High-priority dispatch and starvation avoidance were verified.\n");

    taskforge_pool_destroy(pool);
    printf("[PASS] test_priorities completed successfully!\n\n");
    return 0;
}
