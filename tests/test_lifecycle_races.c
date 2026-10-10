#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

#define SUBMIT_ATTEMPTS 500

typedef struct {
    atomic_int* executed;
    atomic_int* disposed;
    atomic_int* cleanup_calls;
} task_arg_t;

typedef struct {
    taskforge_pool_t* pool;
    taskforge_future_t* futures[SUBMIT_ATTEMPTS];
    size_t future_count;
    atomic_bool start;
    atomic_int attempts;
    atomic_int executed;
    atomic_int disposed;
    atomic_int cleanup_calls;
} submit_race_t;

typedef struct {
    taskforge_pool_t* pool;
    atomic_bool stop;
    atomic_uint iterations;
    atomic_int unexpected_status;
} resize_race_t;

static void* execute_and_dispose(void* raw) {
    task_arg_t* arg = (task_arg_t*)raw;
    usleep(1000);
    atomic_fetch_add(arg->executed, 1);
    atomic_fetch_add(arg->disposed, 1);
    free(arg);
    return NULL;
}

static void cleanup_unexecuted(void* raw) {
    task_arg_t* arg = (task_arg_t*)raw;
    atomic_fetch_add(arg->cleanup_calls, 1);
    atomic_fetch_add(arg->disposed, 1);
    free(arg);
}

static void* submit_until_done(void* raw) {
    submit_race_t* race = (submit_race_t*)raw;
    while (!atomic_load(&race->start)) sched_yield();

    for (size_t i = 0; i < SUBMIT_ATTEMPTS; i++) {
        task_arg_t* arg = (task_arg_t*)malloc(sizeof(*arg));
        assert(arg != NULL);
        arg->executed = &race->executed;
        arg->disposed = &race->disposed;
        arg->cleanup_calls = &race->cleanup_calls;

        taskforge_future_t* future = taskforge_submit_prio_with_cleanup(
            race->pool, execute_and_dispose, arg, TASKFORGE_PRIO_NORMAL,
            cleanup_unexecuted);
        if (future != NULL) race->futures[race->future_count++] = future;
        atomic_fetch_add(&race->attempts, 1);
        if ((i & 7U) == 0) sched_yield();
    }
    return NULL;
}

static void* resize_until_stopped(void* raw) {
    resize_race_t* race = (resize_race_t*)raw;
    size_t target = 1;
    while (!atomic_load(&race->stop)) {
        int status = taskforge_pool_resize(race->pool, target);
        if (status != TASKFORGE_OK && status != TASKFORGE_ERR_SHUTDOWN) {
            atomic_store(&race->unexpected_status, status);
        }
        atomic_fetch_add(&race->iterations, 1);
        target = target == 4 ? 1 : target + 1;
        usleep(500);
    }
    return NULL;
}

static void test_submit_vs_immediate_shutdown(void) {
    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.num_workers = 2;
    config.queue_capacity = 8;
    config.enable_work_stealing = false;

    submit_race_t race = {0};
    atomic_init(&race.start, false);
    atomic_init(&race.attempts, 0);
    atomic_init(&race.executed, 0);
    atomic_init(&race.disposed, 0);
    atomic_init(&race.cleanup_calls, 0);
    race.pool = taskforge_pool_create(&config);
    assert(race.pool != NULL);

    pthread_t submitter;
    assert(pthread_create(&submitter, NULL, submit_until_done, &race) == 0);
    atomic_store(&race.start, true);
    while (atomic_load(&race.attempts) < 32) sched_yield();

    assert(taskforge_pool_shutdown(race.pool, false) == TASKFORGE_OK);
    assert(pthread_join(submitter, NULL) == 0);

    for (size_t i = 0; i < race.future_count; i++) {
        taskforge_future_t* future = race.futures[i];
        taskforge_status_t status = taskforge_future_wait(future, NULL);
        if (status == TASKFORGE_OK) {
            assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_COMPLETED);
        } else {
            assert(status == TASKFORGE_ERR_FAILED);
            assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_FAILED);
            assert(taskforge_future_get_error(future) == TASKFORGE_ERR_SHUTDOWN);
        }
        taskforge_future_release(future);
    }

    assert(atomic_load(&race.attempts) == SUBMIT_ATTEMPTS);
    assert(atomic_load(&race.disposed) == SUBMIT_ATTEMPTS);
    assert(atomic_load(&race.executed) + atomic_load(&race.cleanup_calls) == SUBMIT_ATTEMPTS);

    /* Destruction only begins after the concurrent submitter has joined. */
    taskforge_pool_destroy(race.pool);
    printf("[PASS] concurrent submit/immediate-shutdown terminal states and cleanup\n");
}

static void test_resize_vs_shutdown(void) {
    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.num_workers = 2;
    config.queue_capacity = 8;
    config.enable_work_stealing = false;

    resize_race_t race = {0};
    atomic_init(&race.stop, false);
    atomic_init(&race.iterations, 0);
    atomic_init(&race.unexpected_status, TASKFORGE_OK);
    race.pool = taskforge_pool_create(&config);
    assert(race.pool != NULL);

    pthread_t resizer;
    assert(pthread_create(&resizer, NULL, resize_until_stopped, &race) == 0);
    while (atomic_load(&race.iterations) < 10) sched_yield();

    assert(taskforge_pool_shutdown(race.pool, true) == TASKFORGE_OK);
    atomic_store(&race.stop, true);
    assert(pthread_join(resizer, NULL) == 0);
    assert(atomic_load(&race.iterations) >= 10);
    assert(atomic_load(&race.unexpected_status) == TASKFORGE_OK);

    /* Pool destruction waits until the concurrent resize caller is quiescent. */
    taskforge_pool_destroy(race.pool);
    printf("[PASS] concurrent resize/shutdown serializes without invalid statuses\n");
}

int main(void) {
    alarm(15);
    test_submit_vs_immediate_shutdown();
    test_resize_vs_shutdown();
    puts("[PASS] lifecycle race coverage completed");
    return 0;
}
