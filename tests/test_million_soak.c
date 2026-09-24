#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "taskforge/taskforge.h"

#define DEFAULT_TOTAL_TASKS 1000000
#define NUM_PRODUCERS 4
#define BATCH_SIZE 512

static atomic_uint_fast64_t g_verified_count = 0;
static atomic_uint_fast64_t g_checksum = 0;

static void* compute_soak_fn(void* arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg;
    uint64_t res = (x ^ 0xA5A5A5A55A5A5A5AULL) + 1;
    return (void*)(uintptr_t)res;
}

typedef struct {
    taskforge_pool_t* pool;
    size_t producer_id;
    uint64_t start_val;
    uint64_t count;
} producer_arg_t;

static void* producer_thread(void* arg) {
    producer_arg_t* p = (producer_arg_t*)arg;
    taskforge_future_t* batch[BATCH_SIZE];

    uint64_t current = p->start_val;
    uint64_t remaining = p->count;

    while (remaining > 0) {
        size_t this_batch = (remaining < BATCH_SIZE) ? remaining : BATCH_SIZE;

        for (size_t i = 0; i < this_batch; i++) {
            uint64_t val = current + i;
            batch[i] = taskforge_submit(p->pool, compute_soak_fn, (void*)(uintptr_t)val);
            assert(batch[i] != NULL);
        }

        for (size_t i = 0; i < this_batch; i++) {
            uint64_t val = current + i;
            uint64_t expected = (val ^ 0xA5A5A5A55A5A5A5AULL) + 1;

            void* res = NULL;
            taskforge_status_t s = taskforge_future_wait(batch[i], &res);
            assert(s == TASKFORGE_OK);
            assert((uint64_t)(uintptr_t)res == expected);

            atomic_fetch_add(&g_checksum, (uint64_t)(uintptr_t)res);
            atomic_fetch_add(&g_verified_count, 1);
            taskforge_future_release(batch[i]);
        }

        current += this_batch;
        remaining -= this_batch;
    }

    return NULL;
}

int main(int argc, char** argv) {
    uint64_t total_tasks = DEFAULT_TOTAL_TASKS;
    if (argc > 1) {
        total_tasks = (uint64_t)strtoull(argv[1], NULL, 10);
    }

    printf("=================================================================\n");
    printf("  TaskForge Acceptance Test: %lu Tasks Soak Test\n", (unsigned long)total_tasks);
    printf("  Producers: %d | Worker Threads: 8 | Bounded Queue: 4096\n", NUM_PRODUCERS);
    printf("=================================================================\n");

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 8;
    cfg.queue_capacity = 4096;
    cfg.enable_work_stealing = true;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    pthread_t producers[NUM_PRODUCERS];
    producer_arg_t args[NUM_PRODUCERS];
    uint64_t per_producer = total_tasks / NUM_PRODUCERS;

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        args[i].pool = pool;
        args[i].producer_id = i;
        args[i].start_val = (uint64_t)i * per_producer + 1;
        args[i].count = per_producer;
        pthread_create(&producers[i], NULL, producer_thread, &args[i]);
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    uint64_t total_verified = atomic_load(&g_verified_count);
    printf("  [Verification] Total tasks verified: %lu / %lu\n",
           (unsigned long)total_verified, (unsigned long)total_tasks);
    assert(total_verified == total_tasks);

    taskforge_pool_stats_t stats = taskforge_pool_get_stats(pool);
    printf("  [Pool Stats] Completed: %zu, Stolen: %zu, Rejected: %zu\n",
           stats.completed_tasks, stats.stolen_tasks, stats.rejected_tasks);
    assert(stats.completed_tasks == total_tasks);

    printf("  [Performance] Time: %.3f s | Throughput: %.1f tasks/sec\n",
           elapsed, (double)total_tasks / elapsed);

    /* Graceful teardown */
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);

    printf("[PASS] 100%% of %lu tasks completed with correct results and zero errors!\n\n",
           (unsigned long)total_tasks);
    return 0;
}
