#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include "taskforge/taskforge.h"

#define BENCH_TASKS 100000

static void* short_cpu_work(void* arg) {
    intptr_t v = (intptr_t)arg;
    for (volatile int i = 0; i < 400; i++);
    return (void*)(v + 1);
}

static double run_engine(size_t workers, size_t task_count, bool work_stealing) {
    if (task_count == 0 || task_count > SIZE_MAX / sizeof(taskforge_future_t*)) return 0.0;

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = workers;
    cfg.queue_capacity = 8192;
    cfg.enable_work_stealing = work_stealing;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    taskforge_future_t** futs = malloc(sizeof(*futs) * task_count);
    if (!futs) {
        taskforge_pool_shutdown(pool, false);
        taskforge_pool_destroy(pool);
        return 0.0;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t submitted = 0;
    bool failed = false;
    for (size_t i = 0; i < task_count; i++) {
        futs[i] = taskforge_submit(pool, short_cpu_work, (void*)(intptr_t)i);
        if (!futs[i]) {
            failed = true;
            break;
        }
        submitted++;
    }

    for (size_t i = 0; i < submitted; i++) {
        if (taskforge_future_wait(futs[i], NULL) != TASKFORGE_OK) failed = true;
        taskforge_future_release(futs[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(futs);

    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    taskforge_pool_destroy(pool);

    if (failed || submitted != task_count || sec <= 0.0) return 0.0;
    return (double)task_count / sec;
}

int main(void) {
    size_t task_count = BENCH_TASKS;
    size_t worker_list[] = {2, 4, 8, 12};
    size_t n = sizeof(worker_list) / sizeof(worker_list[0]);

    printf("==========================================================================\n");
    printf("  TaskForge Benchmark: Global Bounded Queue vs Work-Stealing Deques\n");
    printf("  Contention Ceiling Analysis (%zu tasks per run)\n", task_count);
    printf("==========================================================================\n");
    printf("%-10s | %-20s | %-20s | %-12s\n",
           "Workers", "Global Queue (T/s)", "Work-Stealing (T/s)", "Gain");
    printf("--------------------------------------------------------------------------\n");

    for (size_t i = 0; i < n; i++) {
        size_t w = worker_list[i];
        double tp_global = run_engine(w, task_count, false);
        double tp_steal  = run_engine(w, task_count, true);
        if (tp_global <= 0.0 || tp_steal <= 0.0) {
            fprintf(stderr, "Benchmark failed for %zu workers.\n", w);
            return 1;
        }
        double gain = ((tp_steal - tp_global) / tp_global) * 100.0;

        printf("%-10zu | %-20.1f | %-20.1f | %-+.1f%%\n",
               w, tp_global, tp_steal, gain);
    }
    printf("==========================================================================\n");
    return 0;
}
