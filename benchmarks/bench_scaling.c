#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include "taskforge/taskforge.h"

#define BENCH_TASKS 100000

static void* compute_workload(void* arg) {
    intptr_t v = (intptr_t)arg;
    for (volatile int i = 0; i < 800; i++);
    return (void*)(v * 3 + 1);
}

static double run_benchmark(size_t workers, size_t task_count, bool work_stealing) {
    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = workers;
    cfg.queue_capacity = 8192;
    cfg.enable_work_stealing = work_stealing;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    taskforge_future_t** futs = (taskforge_future_t**)malloc(sizeof(taskforge_future_t*) * task_count);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (size_t i = 0; i < task_count; i++) {
        futs[i] = taskforge_submit(pool, compute_workload, (void*)(intptr_t)i);
    }

    for (size_t i = 0; i < task_count; i++) {
        if (futs[i]) {
            taskforge_future_wait(futs[i], NULL);
            taskforge_future_release(futs[i]);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(futs);

    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    taskforge_pool_destroy(pool);

    return (double)task_count / sec;
}

int main(int argc, char** argv) {
    size_t task_count = BENCH_TASKS;
    if (argc > 1) {
        task_count = (size_t)atoi(argv[1]);
    }

    printf("==========================================================================\n");
    printf("  TaskForge Scalability & Contention Analysis Benchmark\n");
    printf("  Tasks per run: %zu\n", task_count);
    printf("==========================================================================\n");
    printf("%-10s | %-16s | %-16s | %-10s\n", "Workers", "Throughput (T/s)", "Speedup (vs 1)", "Efficiency");
    printf("--------------------------------------------------------------------------\n");

    size_t worker_counts[] = {1, 2, 4, 8, 12};
    size_t num_tests = sizeof(worker_counts) / sizeof(worker_counts[0]);

    double baseline_tp = 0.0;
    FILE* csv = fopen("benchmarks/scaling_results.csv", "w");
    if (csv) {
        fprintf(csv, "workers,throughput,speedup,efficiency\n");
    }

    for (size_t i = 0; i < num_tests; i++) {
        size_t w = worker_counts[i];
        double tp = run_benchmark(w, task_count, true);
        if (i == 0) baseline_tp = tp;

        double speedup = tp / baseline_tp;
        double efficiency = (speedup / (double)w) * 100.0;

        printf("%-10zu | %-16.1f | %-16.2fx | %-9.1f%%\n",
               w, tp, speedup, efficiency);

        if (csv) {
            fprintf(csv, "%zu,%.2f,%.2f,%.2f\n", w, tp, speedup, efficiency);
        }
    }

    if (csv) fclose(csv);
    printf("==========================================================================\n");
    printf("Saved CSV data to 'benchmarks/scaling_results.csv'.\n");
    return 0;
}
