#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include "taskforge/taskforge.h"

static void* quick_work(void* arg) {
    intptr_t v = (intptr_t)arg;
    for (volatile int i = 0; i < 500; i++);
    return (void*)(v ^ 0x55);
}

typedef struct {
    taskforge_pool_t* pool;
    int iterations;
    atomic_size_t* accepted;
} stress_thread_arg_t;

static void* stress_producer(void* arg) {
    stress_thread_arg_t* s = (stress_thread_arg_t*)arg;
    for (int i = 0; i < s->iterations; i++) {
        taskforge_future_t* fut = taskforge_submit(s->pool, quick_work, (void*)(intptr_t)i);
        if (fut) {
            atomic_fetch_add(s->accepted, 1);
            if (i % 7 == 0) {
                /* Cancel some tasks randomly */
                taskforge_future_cancel(fut);
            }
            void* res = NULL;
            taskforge_status_t status = taskforge_future_wait(fut, &res);
            if (i % 7 == 0 && status == TASKFORGE_ERR_CANCELLED) {
                assert(taskforge_future_get_state(fut) == TASKFORGE_FUTURE_CANCELLED);
            } else {
                assert(status == TASKFORGE_OK);
                assert((intptr_t)res == (intptr_t)(i ^ 0x55));
            }
            taskforge_future_release(fut);
        }
    }
    return NULL;
}

int main(void) {
    printf("[TEST] Running test_stress (multi-producer concurrency & cancellation churn)...\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 6;
    cfg.queue_capacity = 256;
    cfg.enable_work_stealing = true;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    int num_producers = 4;
    pthread_t producers[num_producers];
    stress_thread_arg_t args[num_producers];
    atomic_size_t accepted;
    atomic_init(&accepted, 0);

    for (int i = 0; i < num_producers; i++) {
        args[i].pool = pool;
        args[i].iterations = 1000;
        args[i].accepted = &accepted;
        assert(pthread_create(&producers[i], NULL, stress_producer, &args[i]) == 0);
    }

    for (int i = 0; i < num_producers; i++) {
        pthread_join(producers[i], NULL);
    }

    assert(atomic_load(&accepted) == (size_t)num_producers * 1000U);

    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);

    printf("[PASS] test_stress finished without deadlocks or race faults!\n\n");
    return 0;
}
