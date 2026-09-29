#include "taskforge/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

static void* execute_parsed_task(void* arg) {
    workload_task_desc_t* desc = (workload_task_desc_t*)arg;
    if (!desc) return NULL;

    if (desc->sleep_ms > 0) {
        usleep(desc->sleep_ms * 1000);
    }

    int64_t val = desc->payload;
    for (uint64_t i = 0; i < desc->compute_iterations; i++) {
        val = (val * 1103515245 + 12345) & 0x7fffffff;
    }

    free(desc);
    return (void*)(intptr_t)val;
}

workload_spec_t* workload_spec_parse_string(const char* text) {
    if (!text) return NULL;

    workload_spec_t* spec = (workload_spec_t*)calloc(1, sizeof(workload_spec_t));
    if (!spec) return NULL;

    spec->capacity = 32;
    spec->tasks = (workload_task_desc_t*)malloc(sizeof(workload_task_desc_t) * spec->capacity);
    if (!spec->tasks) {
        free(spec);
        return NULL;
    }

    char* copy = strdup(text);
    if (!copy) {
        free(spec->tasks);
        free(spec);
        return NULL;
    }

    char* saveptr = NULL;
    char* line = strtok_r(copy, "\r\n", &saveptr);
    uint64_t auto_id = 1;

    while (line) {
        /* Trim leading whitespace */
        while (isspace((unsigned char)*line)) line++;
        if (*line == '\0' || *line == '#') {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char cmd[32] = {0};
        if (sscanf(line, "%31s", cmd) == 1) {
            if (strcmp(cmd, "TASK") == 0) {
                uint64_t tid = 0;
                char prio_str[16] = {0};
                uint32_t sleep_ms = 0;
                uint64_t iters = 0;
                int64_t payload = 0;

                int read_count = sscanf(line, "TASK %lu %15s %u %lu %ld",
                                        &tid, prio_str, &sleep_ms, &iters, &payload);
                if (read_count >= 2) {
                    if (spec->count >= spec->capacity) {
                        size_t new_capacity = spec->capacity * 2;
                        workload_task_desc_t* resized = (workload_task_desc_t*)realloc(spec->tasks, sizeof(workload_task_desc_t) * new_capacity);
                        if (!resized) {
                            free(copy);
                            workload_spec_destroy(spec);
                            return NULL;
                        }
                        spec->tasks = resized;
                        spec->capacity = new_capacity;
                    }
                    workload_task_desc_t* d = &spec->tasks[spec->count++];
                    d->task_id = (tid > 0) ? tid : auto_id++;
                    d->prio = (strcasecmp(prio_str, "HIGH") == 0) ? TASKFORGE_PRIO_HIGH :
                              (strcasecmp(prio_str, "LOW") == 0) ? TASKFORGE_PRIO_LOW : TASKFORGE_PRIO_NORMAL;
                    d->sleep_ms = sleep_ms;
                    d->compute_iterations = iters;
                    d->payload = payload;
                }
            } else if (strcmp(cmd, "REPEAT") == 0) {
                size_t rep = 0;
                char prio_str[16] = {0};
                uint32_t sleep_ms = 0;
                uint64_t iters = 0;
                int64_t payload = 0;

                int read_count = sscanf(line, "REPEAT %zu TASK %15s %u %lu %ld",
                                        &rep, prio_str, &sleep_ms, &iters, &payload);
                if (read_count >= 2) {
                    taskforge_task_priority_t prio = (strcasecmp(prio_str, "HIGH") == 0) ? TASKFORGE_PRIO_HIGH :
                                                     (strcasecmp(prio_str, "LOW") == 0) ? TASKFORGE_PRIO_LOW : TASKFORGE_PRIO_NORMAL;
                    for (size_t r = 0; r < rep; r++) {
                        if (spec->count >= spec->capacity) {
                            size_t new_capacity = (spec->capacity * 2) + rep;
                            workload_task_desc_t* resized = (workload_task_desc_t*)realloc(spec->tasks, sizeof(workload_task_desc_t) * new_capacity);
                            if (!resized) {
                                free(copy);
                                workload_spec_destroy(spec);
                                return NULL;
                            }
                            spec->tasks = resized;
                            spec->capacity = new_capacity;
                        }
                        workload_task_desc_t* d = &spec->tasks[spec->count++];
                        d->task_id = auto_id++;
                        d->prio = prio;
                        d->sleep_ms = sleep_ms;
                        d->compute_iterations = iters;
                        d->payload = payload;
                    }
                }
            }
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(copy);
    return spec;
}

workload_spec_t* workload_spec_parse_file(const char* filepath) {
    if (!filepath) return NULL;
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);

    workload_spec_t* spec = workload_spec_parse_string(buf);
    free(buf);
    return spec;
}

void workload_spec_destroy(workload_spec_t* spec) {
    if (!spec) return;
    free(spec->tasks);
    free(spec);
}

int workload_spec_execute(taskforge_pool_t* pool, const workload_spec_t* spec, bool wait_for_all) {
    if (!pool || !spec) return -1;

    taskforge_future_t** futures = NULL;
    if (wait_for_all) {
        futures = (taskforge_future_t**)calloc(spec->count, sizeof(taskforge_future_t*));
        if (!futures) return -1;
    }

    bool submission_failed = false;
    for (size_t i = 0; i < spec->count; i++) {
        workload_task_desc_t* item = (workload_task_desc_t*)malloc(sizeof(workload_task_desc_t));
        if (!item) {
            submission_failed = true;
            if (wait_for_all && futures) futures[i] = NULL;
            continue;
        }
        *item = spec->tasks[i];

        taskforge_future_t* fut = taskforge_submit_prio(pool, execute_parsed_task, item, item->prio);
        if (!fut) {
            free(item);
            submission_failed = true;
        }

        if (wait_for_all && futures) {
            futures[i] = fut;
        } else if (fut) {
            taskforge_future_release(fut);
        }
    }

    if (wait_for_all && futures) {
        for (size_t i = 0; i < spec->count; i++) {
            if (futures[i]) {
                void* res = NULL;
                taskforge_future_wait(futures[i], &res);
                taskforge_future_release(futures[i]);
            }
        }
        free(futures);
    }

    return submission_failed ? -1 : 0;
}
