#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include "taskforge/taskforge.h"
#include "taskforge/parser.h"

static taskforge_pool_t* g_pool = NULL;
static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

typedef struct {
    int sleep_ms;
    int val;
} cli_task_arg_t;

static void* cli_task_work(void* arg) {
    cli_task_arg_t* t = (cli_task_arg_t*)arg;
    if (t->sleep_ms > 0) {
        usleep(t->sleep_ms * 1000);
    }
    for (volatile int i = 0; i < 50000; i++);
    intptr_t res = t->val * 2;
    free(t);
    return (void*)res;
}

static void* dummy_work(void* arg) {
    intptr_t val = (intptr_t)arg;
    for (volatile int i = 0; i < 100000; i++);
    return (void*)(val * 2);
}

static void print_banner(void) {
    printf("=================================================================\n");
    printf("  TaskForge Engine v1.0 — Thread-Pool Concurrency System\n");
    printf("  C11 (POSIX pthreads) · Bounded Buffer · Work-Stealing · Futures\n");
    printf("=================================================================\n");
    printf("Type 'help' for available commands, 'exit' or Ctrl+C to stop.\n\n");
}

static void print_help(void) {
    printf("Available commands:\n");
    printf("  status                       - Show pool metrics, workers, and tasks\n");
    printf("  submit <prio> <sleep_ms> <val> - Submit a task (prio: low|norm|high)\n");
    printf("  map <count>                  - Run parallel map over N items\n");
    printf("  run <spec_file>              - Execute a workload spec file\n");
    printf("  bench <workers> <tasks>      - Quick scaling benchmark\n");
    printf("  shutdown [graceful|now]      - Shutdown worker pool\n");
    printf("  help                         - Show this help message\n");
    printf("  exit                         - Shutdown and exit\n\n");
}

int main(int argc, char** argv) {
    print_banner();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.queue_capacity = 2048;
    config.enable_priority = true;
    config.enable_work_stealing = true;
    config.log_file_path = "taskforge_tasks.log";

    if (argc > 1) {
        char* end = NULL;
        unsigned long requested = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || requested == 0) {
            fprintf(stderr, "Error: worker count must be a positive integer\n");
            return 1;
        }
        config.num_workers = (size_t)requested;
    }

    printf("[Init] Initializing TaskForge pool (%zu workers, %s priority, %s work-stealing)...\n",
           config.num_workers,
           config.enable_priority ? "enabled" : "disabled",
           config.enable_work_stealing ? "enabled" : "disabled");

    g_pool = taskforge_pool_create(&config);
    if (!g_pool) {
        fprintf(stderr, "Error: Failed to create thread pool\n");
        return 1;
    }

    char line[256];
    while (!g_interrupted) {
        printf("taskforge> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        /* Strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue;

        char cmd[32] = {0};
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "status") == 0) {
            taskforge_pool_stats_t st = taskforge_pool_get_stats(g_pool);
            printf("--- Pool Status ---\n");
            printf("  Workers:   %zu (Active: %zu)\n", st.num_workers, st.active_workers);
            printf("  Queued:    %zu\n", st.queued_tasks);
            printf("  Completed: %zu\n", st.completed_tasks);
            printf("  Rejected:  %zu\n", st.rejected_tasks);
            printf("  Stolen:    %zu\n", st.stolen_tasks);
        } else if (strcmp(cmd, "submit") == 0) {
            char prio_str[16] = "norm";
            int sleep_ms = 0;
            int val = 42;
            sscanf(line, "submit %15s %d %d", prio_str, &sleep_ms, &val);

            taskforge_task_priority_t p = TASKFORGE_PRIO_NORMAL;
            if (strcasecmp(prio_str, "high") == 0) p = TASKFORGE_PRIO_HIGH;
            else if (strcasecmp(prio_str, "low") == 0) p = TASKFORGE_PRIO_LOW;

            cli_task_arg_t* t_arg = (cli_task_arg_t*)malloc(sizeof(cli_task_arg_t));
            if (!t_arg) {
                printf("[Error] Memory allocation failed\n");
                continue;
            }
            t_arg->sleep_ms = sleep_ms;
            t_arg->val = val;

            taskforge_future_t* fut = taskforge_submit_prio(g_pool, cli_task_work, t_arg, p);
            if (!fut) {
                free(t_arg);
                printf("[Error] Submission rejected\n");
            } else {
                printf("[Submitted] Waiting for future...\n");
                void* res = NULL;
                taskforge_status_t s = taskforge_future_wait(fut, &res);
                if (s == TASKFORGE_OK) {
                    printf("[Completed] Result = %ld\n", (long)(intptr_t)res);
                } else {
                    printf("[Failed] Future status: %d\n", s);
                }
                taskforge_future_release(fut);
            }
        } else if (strcmp(cmd, "map") == 0) {
            size_t count = 100;
            sscanf(line, "map %zu", &count);
            if (count == 0) count = 10;

            void** items = (void**)malloc(sizeof(void*) * count);
            void** results = (void**)malloc(sizeof(void*) * count);
            if (!items || !results) {
                printf("[Error] Memory allocation failed for map.\n");
                free(items);
                free(results);
                continue;
            }
            for (size_t i = 0; i < count; i++) items[i] = (void*)(intptr_t)(i + 1);

            printf("[Map] Processing %zu items in parallel...\n", count);
            taskforge_status_t s = taskforge_map(g_pool, dummy_work, items, count, results);
            if (s == TASKFORGE_OK) {
                printf("[Map] All %zu items completed successfully!\n", count);
                printf("  Sample: item[0]->%ld, item[%zu]->%ld\n",
                       (long)(intptr_t)results[0], count - 1, (long)(intptr_t)results[count - 1]);
            } else {
                printf("[Map] Failed with status %d\n", s);
            }
            free(items);
            free(results);
        } else if (strcmp(cmd, "run") == 0) {
            char filepath[128] = {0};
            if (sscanf(line, "run %127s", filepath) == 1) {
                printf("[Parser] Loading spec from '%s'...\n", filepath);
                workload_spec_t* spec = workload_spec_parse_file(filepath);
                if (!spec) {
                    printf("[Error] Could not load spec file\n");
                } else {
                    printf("[Parser] Executing %zu tasks...\n", spec->count);
                    int run_status = workload_spec_execute(g_pool, spec, true);
                    if (run_status == 0) {
                        printf("[Parser] Workload execution completed successfully.\n");
                    } else {
                        printf("[Parser] Workload execution failed with status %d.\n", run_status);
                    }
                    workload_spec_destroy(spec);
                }
            } else {
                printf("Usage: run <filepath>\n");
            }
        } else if (strcmp(cmd, "bench") == 0) {
            size_t workers = config.num_workers;
            size_t tasks = 10000;
            sscanf(line, "bench %zu %zu", &workers, &tasks);

            if (workers == 0 || tasks == 0) {
                printf("[Error] bench requires positive worker and task counts.\n");
                continue;
            }

            taskforge_pool_config_t bench_cfg = config;
            bench_cfg.num_workers = workers;
            bench_cfg.log_file_path = NULL;
            taskforge_pool_t* bench_pool = taskforge_pool_create(&bench_cfg);
            if (!bench_pool) {
                printf("[Error] Failed to create benchmark pool with %zu workers.\n", workers);
                continue;
            }

            printf("[Bench] Running %zu tasks with %zu workers...\n", tasks, workers);
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            if (tasks > SIZE_MAX / sizeof(taskforge_future_t*)) {
                printf("[Error] Task count is too large.\n");
                taskforge_pool_shutdown(bench_pool, false);
                taskforge_pool_destroy(bench_pool);
                continue;
            }

            taskforge_future_t** futs = malloc(sizeof(*futs) * tasks);
            if (!futs) {
                printf("[Error] Memory allocation failed for benchmark.\n");
                taskforge_pool_shutdown(bench_pool, false);
                taskforge_pool_destroy(bench_pool);
                continue;
            }

            size_t submitted = 0;
            bool failed = false;
            for (size_t i = 0; i < tasks; i++) {
                futs[i] = taskforge_submit(bench_pool, dummy_work, (void*)(intptr_t)i);
                if (!futs[i]) {
                    failed = true;
                    break;
                }
                submitted++;
            }

            for (size_t i = 0; i < submitted; i++) {
                taskforge_status_t wait_status = taskforge_future_wait(futs[i], NULL);
                if (wait_status != TASKFORGE_OK) failed = true;
                taskforge_future_release(futs[i]);
            }
            free(futs);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (failed || submitted != tasks || sec <= 0.0) {
                printf("[Bench] Benchmark failed: only %zu/%zu tasks completed successfully.\n", submitted, tasks);
            } else {
                printf("[Bench] Completed %zu tasks in %.4f s (%.1f tasks/sec)\n",
                       tasks, sec, (double)tasks / sec);
            }

            taskforge_pool_shutdown(bench_pool, true);
            taskforge_pool_destroy(bench_pool);
        } else if (strcmp(cmd, "shutdown") == 0) {
            char mode[16] = "graceful";
            sscanf(line, "shutdown %15s", mode);
            bool graceful = (strcmp(mode, "now") != 0);
            printf("[Shutdown] Shutting down pool (%s)...\n", graceful ? "graceful" : "immediate");
            taskforge_pool_shutdown(g_pool, graceful);
            printf("[Shutdown] Done.\n");
        } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else {
            printf("Unknown command '%s'. Type 'help' for options.\n", cmd);
        }
    }

    if (g_interrupted) {
        printf("\n[Signal] Interrupted by SIGINT (Ctrl+C). Initiating graceful teardown...\n");
    }

    printf("[Cleanup] Draining tasks, joining threads, and freeing resources...\n");
    taskforge_pool_shutdown(g_pool, true);
    taskforge_pool_destroy(g_pool);
    printf("[Done] TaskForge exited cleanly.\n");
    return 0;
}
