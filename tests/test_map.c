#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

static taskforge_pool_t* g_nested_pool = NULL;

static void* nested_map_task(void* arg) {
    intptr_t v = (intptr_t)arg;
    return (void*)(v + 10);
}

static void* reentrant_map_task(void* arg) {
    (void)arg;
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

static int status_task(void* arg, void** result) {
    intptr_t value = (intptr_t)arg;
    if (value < 0) return 77;
    *result = (void*)(value * 2);
    return 0;
}

static void* slow_map_task(void* arg) {
    usleep(50000);
    return arg;
}

static atomic_bool map_first_started = false;

static void* shutdown_race_map_task(void* arg) {
    if ((intptr_t)arg == 1) {
        atomic_store(&map_first_started, true);
        usleep(100000);
    }
    return arg;
}

static void* shutdown_map_pool(void* arg) {
    taskforge_pool_t* pool = (taskforge_pool_t*)arg;
    while (!atomic_load(&map_first_started)) usleep(100);
    usleep(10000);
    assert(taskforge_pool_shutdown(pool, false) == TASKFORGE_OK);
    return NULL;
}

int main(void) {
    alarm(15);
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

    /* Regression: map must snapshot inputs before clearing aliased results. */
    void* in_place[4] = {(void*)1, (void*)2, (void*)3, (void*)4};
    assert(taskforge_map(pool, multiply_by_five, in_place, 4, in_place) == TASKFORGE_OK);
    for (size_t i = 0; i < 4; i++) {
        assert((intptr_t)in_place[i] == (intptr_t)(i + 1) * 5);
    }
    printf("  [PASS] In-place taskforge_map preserves aliased input items.\n");

    taskforge_future_t* status_future = taskforge_submit_status(pool, status_task, (void*)(intptr_t)-1);
    assert(status_future != NULL);
    assert(taskforge_future_wait(status_future, NULL) == TASKFORGE_ERR_FAILED);
    assert(taskforge_future_get_error(status_future) == 77);
    taskforge_future_release(status_future);
    printf("  [PASS] Structured task errors are preserved by futures.\n");

    void* slow_items[2] = {(void*)1, (void*)2};
    taskforge_map_item_result_t report[2];
    taskforge_pool_config_t timeout_cfg = cfg;
    timeout_cfg.num_workers = 1;
    timeout_cfg.queue_capacity = 8;
    taskforge_pool_t* timeout_pool = taskforge_pool_create(&timeout_cfg);
    assert(timeout_pool != NULL);
    assert(taskforge_map_timeout_report(timeout_pool, slow_map_task, slow_items, 2, report, 1) == TASKFORGE_ERR_TIMEOUT);
    assert(report[0].status == TASKFORGE_ERR_TIMEOUT);
    assert(report[1].status == TASKFORGE_ERR_CANCELLED);
    taskforge_pool_destroy(timeout_pool);
    printf("  [PASS] Map timeout reports the timed-out item and canceled queued items.\n");

    /* If a later submission is rejected during shutdown, earlier report entries
     * must reflect their actual future states instead of the initial failure value. */
    taskforge_pool_config_t submit_race_cfg = cfg;
    submit_race_cfg.num_workers = 1;
    submit_race_cfg.queue_capacity = 1;
    taskforge_pool_t* submit_race_pool = taskforge_pool_create(&submit_race_cfg);
    assert(submit_race_pool != NULL);
    atomic_store(&map_first_started, false);
    pthread_t shutdown_thread;
    assert(pthread_create(&shutdown_thread, NULL, shutdown_map_pool, submit_race_pool) == 0);
    void* race_items[3] = {(void*)1, (void*)2, (void*)3};
    taskforge_map_item_result_t race_report[3];
    taskforge_status_t race_status = taskforge_map_timeout_report(
        submit_race_pool, shutdown_race_map_task, race_items, 3, race_report, 5000);
    assert(pthread_join(shutdown_thread, NULL) == 0);
    assert(race_status != TASKFORGE_OK);
    assert(race_report[0].status == TASKFORGE_OK);
    assert(race_report[0].result == (void*)1);
    assert(race_report[1].status == TASKFORGE_ERR_FAILED);
    assert(race_report[1].task_error == TASKFORGE_ERR_SHUTDOWN);
    assert(race_report[2].status == TASKFORGE_ERR_FAILED);
    taskforge_pool_destroy(submit_race_pool);
    puts("  [PASS] Map submission failure reports prior futures accurately.");

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
