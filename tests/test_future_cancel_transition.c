#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "taskforge/taskforge.h"

typedef struct {
    atomic_bool started;
    atomic_bool release;
    atomic_int executed;
    atomic_int cleanup_calls;
    atomic_int disposed;
} running_cancel_t;

typedef struct { running_cancel_t* state; } task_arg_t;

static void* block_while_running(void* raw) {
    task_arg_t* arg = (task_arg_t*)raw;
    running_cancel_t* state = arg->state;
    atomic_store(&state->started, true);
    while (!atomic_load(&state->release)) sched_yield();
    atomic_fetch_add(&state->executed, 1);
    atomic_fetch_add(&state->disposed, 1);
    free(arg);
    return NULL;
}

static void cleanup_unstarted(void* raw) {
    task_arg_t* arg = (task_arg_t*)raw;
    atomic_fetch_add(&arg->state->cleanup_calls, 1);
    atomic_fetch_add(&arg->state->disposed, 1);
    free(arg);
}

int main(void) {
    alarm(15);
    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.num_workers = 1;
    config.queue_capacity = 4;
    config.enable_work_stealing = false;
    taskforge_pool_t* pool = taskforge_pool_create(&config);
    assert(pool != NULL);

    running_cancel_t state = {0};
    atomic_init(&state.started, false);
    atomic_init(&state.release, false);
    atomic_init(&state.executed, 0);
    atomic_init(&state.cleanup_calls, 0);
    atomic_init(&state.disposed, 0);

    task_arg_t* arg = (task_arg_t*)malloc(sizeof(*arg));
    assert(arg != NULL);
    arg->state = &state;
    taskforge_future_t* future = taskforge_submit_prio_with_cleanup(
        pool, block_while_running, arg, TASKFORGE_PRIO_NORMAL, cleanup_unstarted);
    assert(future != NULL);

    /* The callback sets started only after the worker marks the future RUNNING. */
    while (!atomic_load(&state.started)) sched_yield();
    assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_RUNNING);
    assert(!taskforge_future_cancel(future));

    atomic_store(&state.release, true);
    assert(taskforge_future_wait(future, NULL) == TASKFORGE_OK);
    assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_COMPLETED);
    assert(atomic_load(&state.executed) == 1);
    assert(atomic_load(&state.cleanup_calls) == 0);
    assert(atomic_load(&state.disposed) == 1);
    taskforge_future_release(future);

    assert(taskforge_pool_shutdown(pool, true) == TASKFORGE_OK);
    taskforge_pool_destroy(pool);
    puts("[PASS] cancellation after transition to RUNNING is rejected without duplicate cleanup");
    return 0;
}
