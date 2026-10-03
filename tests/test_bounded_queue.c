#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include "taskforge/queue.h"
#include "taskforge/future.h"

#define TEST_CAPACITY 8

static void* dummy_task(void* arg) {
    return arg;
}

typedef struct {
    taskforge_queue_t* q;
    int items_to_consume;
    int consumed_count;
} consumer_arg_t;

static void* consumer_thread(void* arg) {
    consumer_arg_t* c = (consumer_arg_t*)arg;
    for (int i = 0; i < c->items_to_consume; i++) {
        taskforge_task_t t;
        bool ok = queue_pop(c->q, &t);
        if (ok) {
            c->consumed_count++;
            if (t.future) {
                future_release(t.future);
                future_release(t.future);
            }
        }
    }
    return NULL;
}

int main(void) {
    printf("[TEST] Running test_bounded_queue...\n");
    assert(queue_create(SIZE_MAX, false) == NULL);
    printf("  [PASS] Queue rejects allocation-size overflow.\n");

    taskforge_queue_t* q = queue_create(TEST_CAPACITY, false);
    assert(q != NULL);
    assert(queue_is_empty(q));

    /* 1. Test filling queue to capacity */
    for (int i = 0; i < TEST_CAPACITY; i++) {
        taskforge_task_t t = {
            .task_id = i + 1,
            .fn = dummy_task,
            .arg = (void*)(intptr_t)i,
            .future = future_create(i + 1),
            .prio = TASKFORGE_PRIO_NORMAL
        };
        taskforge_status_t s = queue_try_push(q, &t);
        assert(s == TASKFORGE_OK);
    }
    assert(queue_size(q) == TEST_CAPACITY);

    /* 2. Test backpressure: try_push must fail when full */
    taskforge_task_t overflow_task = {
        .task_id = 999,
        .fn = dummy_task,
        .arg = NULL,
        .future = future_create(999),
        .prio = TASKFORGE_PRIO_NORMAL
    };
    taskforge_status_t s = queue_try_push(q, &overflow_task);
    assert(s == TASKFORGE_ERR_FULL);
    printf("  [PASS] Queue backpressure signaled TASKFORGE_ERR_FULL when capacity reached.\n");
    future_release(overflow_task.future);
    future_release(overflow_task.future);

    /* 3. Test timed push */
    taskforge_task_t timeout_task = {
        .task_id = 1000,
        .fn = dummy_task,
        .arg = NULL,
        .future = future_create(1000),
        .prio = TASKFORGE_PRIO_NORMAL
    };
    s = queue_push_timeout(q, &timeout_task, 50); /* 50ms timeout */
    assert(s == TASKFORGE_ERR_TIMEOUT);
    printf("  [PASS] queue_push_timeout returned TASKFORGE_ERR_TIMEOUT on full queue.\n");
    future_release(timeout_task.future);
    future_release(timeout_task.future);

    /* 4. Test blocking unblock: start consumer thread to pop */
    consumer_arg_t c_arg = { .q = q, .items_to_consume = TEST_CAPACITY, .consumed_count = 0 };
    pthread_t c_tid;
    assert(pthread_create(&c_tid, NULL, consumer_thread, &c_arg) == 0);

    assert(pthread_join(c_tid, NULL) == 0);
    assert(c_arg.consumed_count == TEST_CAPACITY);
    assert(queue_is_empty(q));
    printf("  [PASS] Consumer drained all %d items, queue is now empty.\n", TEST_CAPACITY);

    queue_destroy(q);
    printf("[PASS] test_bounded_queue completed successfully!\n\n");
    return 0;
}
