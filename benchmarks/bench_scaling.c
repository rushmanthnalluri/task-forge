#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include "taskforge/taskforge.h"

#define BENCH_TASKS 100000
#define BENCH_REPEATS 3

static void* compute_workload(void* arg) {
    intptr_t v = (intptr_t)arg;
    for (volatile int i = 0; i < 800; i++);
    return (void*)(v * 3 + 1);
}

static double run_benchmark(size_t workers, size_t task_count, bool work_stealing) {
    if (task_count == 0 || task_count > SIZE_MAX / sizeof(taskforge_future_t*)) return 0.0;

    double total_throughput = 0.0;
    for (size_t repeat = 0; repeat < BENCH_REPEATS; repeat++) {
        taskforge_pool_config_t cfg;
        taskforge_default_config(&cfg);
        cfg.num_workers = workers;
        cfg.queue_capacity = 8192;
        cfg.enable_work_stealing = work_stealing;

        taskforge_pool_t* pool = taskforge_pool_create(&cfg);
        if (!pool) return 0.0;

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
            futs[i] = taskforge_submit(pool, compute_workload, (void*)(intptr_t)i);
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
        taskforge_pool_shutdown(pool, true);
        taskforge_pool_destroy(pool);

        if (failed || submitted != task_count || sec <= 0.0) return 0.0;
        total_throughput += (double)task_count / sec;
    }

    return total_throughput / (double)BENCH_REPEATS;
}

int main(int argc, char** argv) {
    size_t task_count = BENCH_TASKS;
    if (argc > 1) {
        char* end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || errno == ERANGE || parsed == 0 || parsed > SIZE_MAX) {
            fprintf(stderr, "Usage: %s [positive-task-count]\n", argv[0]);
            return 2;
        }
        task_count = (size_t)parsed;
    }

    printf("==========================================================================\n");
    printf("  TaskForge Scalability & Contention Analysis Benchmark\n");
    printf("  Tasks per run: %zu | Repeats: %d\n", task_count, BENCH_REPEATS);
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
        if (tp <= 0.0) {
            fprintf(stderr, "Benchmark failed for %zu workers.\n", w);
            if (csv) fclose(csv);
            return 1;
        }
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
