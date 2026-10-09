#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

static taskforge_pool_t* pool;
static atomic_int completed;
static void* unit(void* arg) { (void)arg; atomic_fetch_add(&completed, 1); return NULL; }

static void* resizer(void* arg) {
    size_t base = (size_t)(intptr_t)arg;
    for (int i = 0; i < 100; i++) {
        size_t count = base + (size_t)(i % 4);
        int rc = taskforge_pool_resize(pool, count);
        assert(rc == TASKFORGE_OK || rc == TASKFORGE_ERR_SHUTDOWN);
    }
    return NULL;
}

int main(void) {
    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 2;
    cfg.queue_capacity = 256;
    cfg.enable_work_stealing = true;
    pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);
    pthread_t a, b;
    assert(pthread_create(&a, NULL, resizer, (void*)(intptr_t)1) == 0);
    assert(pthread_create(&b, NULL, resizer, (void*)(intptr_t)2) == 0);
    taskforge_future_t* futures[500];
    for (size_t i = 0; i < 500; i++) {
        futures[i] = taskforge_submit(pool, unit, NULL);
        assert(futures[i] != NULL);
    }
    assert(pthread_join(a, NULL) == 0);
    assert(pthread_join(b, NULL) == 0);
    for (size_t i = 0; i < 500; i++) {
        assert(taskforge_future_wait(futures[i], NULL) == TASKFORGE_OK);
        taskforge_future_release(futures[i]);
    }
    assert(atomic_load(&completed) == 500);
    assert(taskforge_pool_resize(pool, 1) == TASKFORGE_OK);
    assert(taskforge_pool_worker_count(pool) == 1);
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);
    puts("[PASS] test_resize_stress completed concurrent resize and submission coverage.");
    return 0;
}
