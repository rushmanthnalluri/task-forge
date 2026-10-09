#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include "taskforge/taskforge.h"

static taskforge_pool_t* pool;
static atomic_int completed;
static atomic_int observers_entered;
static void* unit(void* arg) { (void)arg; atomic_fetch_add(&completed, 1); return NULL; }

static void* resize_observer(void* arg) {
    (void)arg;
    atomic_fetch_add(&observers_entered, 1);
    while (taskforge_pool_get_stats(pool).num_workers > 1) usleep(1000);
    assert(taskforge_pool_worker_count(pool) == 1);
    (void)taskforge_pool_get_stats(pool);
    return NULL;
}

static void* shrink_to_one(void* arg) {
    int* rc = (int*)arg;
    *rc = taskforge_pool_resize(pool, 1);
    return NULL;
}

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
    alarm(30);
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
    /* Regression: callbacks query pool state while shrink joins workers. */
    assert(taskforge_pool_resize(pool, 2) == TASKFORGE_OK);
    atomic_store(&observers_entered, 0);
    taskforge_future_t* observers[2];
    for (size_t i = 0; i < 2; i++) {
        observers[i] = taskforge_submit(pool, resize_observer, NULL);
        assert(observers[i] != NULL);
    }
    for (int i = 0; i < 5000 && atomic_load(&observers_entered) < 2; i++) usleep(1000);
    assert(atomic_load(&observers_entered) == 2);
    int shrink_rc = TASKFORGE_ERR_FAILED;
    pthread_t shrink_thread;
    assert(pthread_create(&shrink_thread, NULL, shrink_to_one, &shrink_rc) == 0);
    assert(pthread_join(shrink_thread, NULL) == 0);
    assert(shrink_rc == TASKFORGE_OK);
    for (size_t i = 0; i < 2; i++) {
        assert(taskforge_future_wait(observers[i], NULL) == TASKFORGE_OK);
        taskforge_future_release(observers[i]);
    }
    assert(taskforge_pool_worker_count(pool) == 1);
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);
    puts("[PASS] test_resize_stress completed concurrent resize and submission coverage.");
    return 0;
}
