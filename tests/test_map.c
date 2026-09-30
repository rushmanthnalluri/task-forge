#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "taskforge/taskforge.h"

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

    free(items);
    free(results);
    taskforge_pool_destroy(pool);
    printf("[PASS] test_map completed successfully!\n\n");
    return 0;
}
