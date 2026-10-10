#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

static atomic_int ran;
static void* work(void* arg) { (void)arg; usleep(1000); atomic_fetch_add(&ran, 1); return NULL; }

int main(void) {
    alarm(15);
    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 2;
    cfg.queue_capacity = 128;
    cfg.enable_work_stealing = true;
    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL && taskforge_pool_worker_count(pool) == 2);
    assert(taskforge_pool_resize(pool, 4) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(pool) == 4);
    taskforge_future_t* futures[64];
    for (size_t i = 0; i < 64; i++) { futures[i] = taskforge_submit(pool, work, NULL); assert(futures[i]); }
    assert(taskforge_pool_resize(pool, 1) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(pool) == 1);
    for (size_t i = 0; i < 64; i++) { assert(taskforge_future_wait(futures[i], NULL) == TASKFORGE_OK); taskforge_future_release(futures[i]); }
    assert(atomic_load(&ran) == 64);
    assert(taskforge_pool_resize(pool, 3) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(pool) == 3);
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);

    /* Global-queue workers must observe idle retirement requests during shrink. */
    taskforge_pool_config_t global_cfg;
    taskforge_default_config(&global_cfg);
    global_cfg.num_workers = 2;
    global_cfg.queue_capacity = 128;
    global_cfg.enable_work_stealing = false;
    taskforge_pool_t* global_pool = taskforge_pool_create(&global_cfg);
    assert(global_pool != NULL && taskforge_pool_worker_count(global_pool) == 2);
    assert(taskforge_pool_resize(global_pool, 1) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(global_pool) == 1);
    assert(taskforge_pool_resize(global_pool, 2) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(global_pool) == 2);
    assert(taskforge_pool_shutdown(global_pool, true) == TASKFORGE_OK);
    taskforge_pool_destroy(global_pool);
    printf("[PASS] global-queue idle workers retire and regrow.\n");

    printf("[PASS] test_resize completed growth, shrinkage, queued completion, and regrowth.\n");
    return 0;
}
