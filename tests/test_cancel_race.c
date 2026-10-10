#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

#define CANCEL_RACE_ITERATIONS 200

typedef struct {
    atomic_bool started;
    atomic_bool release;
} blocker_gate_t;

typedef struct {
    atomic_bool started;
    atomic_bool release;
    atomic_int executed;
    atomic_int cleaned;
    atomic_int disposed;
} cancel_target_state_t;

typedef struct {
    cancel_target_state_t* state;
} cancel_target_arg_t;

typedef struct {
    atomic_bool start;
    atomic_bool cancel_done;
    blocker_gate_t* blocker;
    cancel_target_state_t* target;
    taskforge_future_t* future;
    int mode; /* 0: cancel pending first, 1: cancel while RUNNING, 2: race */
    unsigned cancel_delay;
    unsigned release_delay;
    bool cancel_result;
} cancel_race_control_t;

typedef struct {
    atomic_bool* start;
    taskforge_pool_t* pool;
    int result;
} shutdown_control_t;

static void* blocker_task(void* opaque) {
    blocker_gate_t* gate = (blocker_gate_t*)opaque;
    atomic_store(&gate->started, true);
    while (!atomic_load(&gate->release)) {
        sched_yield();
    }
    return NULL;
}

static void* cancel_target_task(void* opaque) {
    cancel_target_arg_t* arg = (cancel_target_arg_t*)opaque;
    cancel_target_state_t* state = arg->state;
    atomic_store(&state->started, true);
    while (!atomic_load(&state->release)) {
        sched_yield();
    }
    atomic_fetch_add(&state->executed, 1);
    atomic_fetch_add(&state->disposed, 1);
    free(arg);
    return NULL;
}

static void cancel_target_cleanup(void* opaque) {
    cancel_target_arg_t* arg = (cancel_target_arg_t*)opaque;
    atomic_fetch_add(&arg->state->cleaned, 1);
    atomic_fetch_add(&arg->state->disposed, 1);
    free(arg);
}

static void* cancel_thread(void* opaque) {
    cancel_race_control_t* control = (cancel_race_control_t*)opaque;
    while (!atomic_load(&control->start)) {
        sched_yield();
    }
    if (control->mode == 1) {
        /* The callback sets started only after future_mark_running succeeds. */
        while (!atomic_load(&control->target->started)) {
            sched_yield();
        }
    }
    for (unsigned i = 0; i < control->cancel_delay; i++) {
        sched_yield();
    }
    control->cancel_result = taskforge_future_cancel(control->future);
    atomic_store(&control->cancel_done, true);
    return NULL;
}

static void* release_blocker_thread(void* opaque) {
    cancel_race_control_t* control = (cancel_race_control_t*)opaque;
    while (!atomic_load(&control->start)) {
        sched_yield();
    }
    if (control->mode == 0) {
        /* Deterministically exercise cancellation winning while still pending. */
        while (!atomic_load(&control->cancel_done)) {
            sched_yield();
        }
    }
    for (unsigned i = 0; i < control->release_delay; i++) {
        sched_yield();
    }
    atomic_store(&control->blocker->release, true);
    return NULL;
}

static void* immediate_shutdown_thread(void* opaque) {
    shutdown_control_t* control = (shutdown_control_t*)opaque;
    while (!atomic_load(control->start)) {
        sched_yield();
    }
    control->result = taskforge_pool_shutdown(control->pool, false);
    return NULL;
}

static taskforge_pool_t* make_single_worker_pool(void) {
    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1;
    cfg.queue_capacity = 4;
    cfg.enable_work_stealing = false;
    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);
    return pool;
}

static void init_target_state(cancel_target_state_t* state) {
    atomic_init(&state->started, false);
    atomic_init(&state->release, false);
    atomic_init(&state->executed, 0);
    atomic_init(&state->cleaned, 0);
    atomic_init(&state->disposed, 0);
}

static taskforge_future_t* submit_blocker(taskforge_pool_t* pool, blocker_gate_t* gate) {
    atomic_init(&gate->started, false);
    atomic_init(&gate->release, false);
    taskforge_future_t* future = taskforge_submit(pool, blocker_task, gate);
    assert(future != NULL);
    while (!atomic_load(&gate->started)) {
        sched_yield();
    }
    return future;
}

static taskforge_future_t* submit_cancel_target(taskforge_pool_t* pool,
                                                cancel_target_state_t* state) {
    cancel_target_arg_t* arg = malloc(sizeof(*arg));
    assert(arg != NULL);
    arg->state = state;
    taskforge_future_t* future = taskforge_submit_prio_with_cleanup(
        pool, cancel_target_task, arg, TASKFORGE_PRIO_NORMAL, cancel_target_cleanup);
    assert(future != NULL);
    return future;
}

static void assert_cancel_outcome(taskforge_future_t* future,
                                  cancel_target_state_t* state,
                                  bool cancel_result) {
    taskforge_status_t status = taskforge_future_wait(future, NULL);
    if (cancel_result) {
        assert(status == TASKFORGE_ERR_CANCELLED);
        assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_CANCELLED);
        assert(atomic_load(&state->executed) == 0);
        assert(atomic_load(&state->cleaned) == 1);
    } else {
        assert(status == TASKFORGE_OK);
        assert(taskforge_future_get_state(future) == TASKFORGE_FUTURE_COMPLETED);
        assert(atomic_load(&state->executed) == 1);
        assert(atomic_load(&state->cleaned) == 0);
    }
    assert(atomic_load(&state->disposed) == 1);
    assert(atomic_load(&state->executed) + atomic_load(&state->cleaned) == 1);
}

static void test_cancel_vs_running_transition(void) {
    taskforge_pool_t* pool = make_single_worker_pool();
    int cancel_wins = 0;
    int worker_wins = 0;

    for (int i = 0; i < CANCEL_RACE_ITERATIONS; i++) {
        blocker_gate_t blocker;
        cancel_target_state_t target;
        init_target_state(&target);
        taskforge_future_t* blocker_future = submit_blocker(pool, &blocker);
        taskforge_future_t* target_future = submit_cancel_target(pool, &target);

        cancel_race_control_t control = {0};
        atomic_init(&control.start, false);
        atomic_init(&control.cancel_done, false);
        control.blocker = &blocker;
        control.target = &target;
        control.future = target_future;
        control.mode = i % 3;
        control.cancel_delay = control.mode == 2 ? (unsigned)((i * 3) % 17) : 0;
        control.release_delay = control.mode == 2 ? (unsigned)((i * 7) % 17) : 0;

        pthread_t canceller, releaser;
        assert(pthread_create(&canceller, NULL, cancel_thread, &control) == 0);
        assert(pthread_create(&releaser, NULL, release_blocker_thread, &control) == 0);
        atomic_store(&control.start, true);
        assert(pthread_join(releaser, NULL) == 0);
        assert(pthread_join(canceller, NULL) == 0);

        /* A running callback is allowed to finish; cancelled pending work is cleaned. */
        atomic_store(&target.release, true);
        assert(taskforge_future_wait(blocker_future, NULL) == TASKFORGE_OK);
        assert_cancel_outcome(target_future, &target, control.cancel_result);
        if (control.cancel_result) cancel_wins++;
        else worker_wins++;
        taskforge_future_release(blocker_future);
        taskforge_future_release(target_future);
    }

    assert(cancel_wins > 0);
    assert(worker_wins > 0);
    taskforge_pool_destroy(pool);
    printf("  [PASS] Cancellation-vs-RUNNING transition: %d cancellations won, %d tasks ran.\n",
           cancel_wins, worker_wins);
}

static void test_cancel_vs_immediate_shutdown(void) {
    taskforge_pool_t* pool = make_single_worker_pool();
    blocker_gate_t blocker;
    cancel_target_state_t target;
    init_target_state(&target);
    taskforge_future_t* blocker_future = submit_blocker(pool, &blocker);
    taskforge_future_t* target_future = submit_cancel_target(pool, &target);

    cancel_race_control_t cancel = {0};
    atomic_init(&cancel.start, false);
    atomic_init(&cancel.cancel_done, false);
    cancel.blocker = &blocker;
    cancel.target = &target;
    cancel.future = target_future;
    cancel.mode = 2;
    cancel.cancel_delay = 8;
    cancel.release_delay = 8;

    shutdown_control_t shutdown = {0};
    shutdown.start = &cancel.start;
    shutdown.pool = pool;
    shutdown.result = TASKFORGE_ERR_INVALID;

    pthread_t canceller, releaser, shutdownter;
    assert(pthread_create(&canceller, NULL, cancel_thread, &cancel) == 0);
    assert(pthread_create(&releaser, NULL, release_blocker_thread, &cancel) == 0);
    assert(pthread_create(&shutdownter, NULL, immediate_shutdown_thread, &shutdown) == 0);
    atomic_store(&cancel.start, true);

    assert(pthread_join(releaser, NULL) == 0);
    assert(pthread_join(canceller, NULL) == 0);
    /* Unblock a task if it won the dequeue race before immediate shutdown. */
    atomic_store(&target.release, true);
    assert(pthread_join(shutdownter, NULL) == 0);
    assert(shutdown.result == TASKFORGE_OK);
    assert(taskforge_future_wait(blocker_future, NULL) == TASKFORGE_OK);

    taskforge_status_t status = taskforge_future_wait(target_future, NULL);
    taskforge_future_state_t state = taskforge_future_get_state(target_future);
    if (state == TASKFORGE_FUTURE_CANCELLED) {
        assert(status == TASKFORGE_ERR_CANCELLED);
        assert(atomic_load(&target.executed) == 0);
        assert(atomic_load(&target.cleaned) == 1);
    } else if (state == TASKFORGE_FUTURE_FAILED) {
        assert(status == TASKFORGE_ERR_FAILED);
        assert(taskforge_future_get_error(target_future) == TASKFORGE_ERR_SHUTDOWN);
        assert(atomic_load(&target.executed) == 0);
        assert(atomic_load(&target.cleaned) == 1);
    } else {
        assert(state == TASKFORGE_FUTURE_COMPLETED);
        assert(status == TASKFORGE_OK);
        assert(atomic_load(&target.executed) == 1);
        assert(atomic_load(&target.cleaned) == 0);
    }
    assert(atomic_load(&target.disposed) == 1);
    assert(atomic_load(&target.executed) + atomic_load(&target.cleaned) == 1);

    taskforge_future_release(blocker_future);
    taskforge_future_release(target_future);
    taskforge_pool_destroy(pool);
    printf("  [PASS] Cancellation racing immediate shutdown leaves one terminal future and exactly-once disposal.\n");
}

int main(void) {
    alarm(30);
    printf("[TEST] Running cancellation transition race regressions...\n");
    test_cancel_vs_running_transition();
    test_cancel_vs_immediate_shutdown();
    printf("[PASS] test_cancel_race completed successfully.\n");
    return 0;
}
