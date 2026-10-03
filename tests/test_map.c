#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "taskforge/taskforge.h"

static taskforge_pool_t* g_nested_pool = NULL;

static void* nested_map_task(void* arg) {
    intptr_t v = (intptr_t)arg;
    return (void*)(v + 10);
}

static void* reentrant_map_task(void* arg) {
    void** results = calloc(3, sizeof(*results));
    void* items[3] = {(void*)1, (void*)2, (void*)3};
    assert(results != NULL);
    taskforge_status_t s = taskforge_map(g_nested_pool, nested_map_task, items, 3, results);
    assert(s == TASKFORGE_OK);
    assert((intptr_t)results[0] == 11);
    assert((intptr_t)results[1] == 12);
    assert((intptr_t)results[2] == 13);
    free(results);
    return (void*)1;
}

static void* multiply_by_five(void* arg) {
    intptr_t v = (intptr_t)arg;
    return (void*)(v * 5);
}

int main(void) {
    printf("[TEST] Running test_map...\n");
    assert(taskforge_map(NULL, NULL, NULL, 0, NULL) == TASKFORGE_ERR_INVALID);

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 4;
    cfg.queue_capacity = 2048;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);
    assert(taskforge_map(pool, multiply_by_five, NULL, 0, NULL) == TASKFORGE_OK);

    size_t count = 1000;
    void** items = (void**)malloc(sizeof(void*) * count);
    void** results = (void**)malloc(sizeof(void*) * count);

    for (size_t i = 0; i < count; i++) {
        items[i] = (void*)(intptr_t)(i + 1);
    }

    taskforge_status_t s = taskforge_map(pool, multiply_by_five, items, count, results);
    assert(s == TASKFORGE_OK);

    for (size_t i = 0; i < count; i++) {
        intptr_t expected = (intptr_t)(i + 1) * 5;
        assert((intptr_t)results[i] == expected);
    }
    printf("  [PASS] taskforge_map mapped %zu elements correctly in parallel.\n", count);

    /*
     * Regression: a taskforge_map call from a worker must not deadlock when
     * that worker is the only worker available to execute the nested tasks.
     */
    taskforge_pool_config_t nested_cfg = cfg;
    nested_cfg.num_workers = 1;
    taskforge_pool_t* nested_pool = taskforge_pool_create(&nested_cfg);
    assert(nested_pool != NULL);
    g_nested_pool = nested_pool;
    taskforge_future_t* nested_future = taskforge_submit(nested_pool, reentrant_map_task, NULL);
    assert(nested_future != NULL);
    assert(taskforge_future_wait(nested_future, NULL) == TASKFORGE_OK);
    taskforge_future_release(nested_future);
    taskforge_pool_shutdown(nested_pool, true);
    taskforge_pool_destroy(nested_pool);
    g_nested_pool = NULL;
    printf("  [PASS] Reentrant taskforge_map avoids single-worker deadlock.\n");

    free(items);
    free(results);
    taskforge_pool_destroy(pool);
    printf("[PASS] test_map completed successfully!\n\n");
    return 0;
}
