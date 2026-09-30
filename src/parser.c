#include "taskforge/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <inttypes.h>

static bool parse_priority(const char* text, taskforge_task_priority_t* out) {
    if (!text || !out) return false;
    if (strcasecmp(text, "HIGH") == 0) {
        *out = TASKFORGE_PRIO_HIGH;
        return true;
    }
    if (strcasecmp(text, "NORMAL") == 0 || strcasecmp(text, "NORM") == 0) {
        *out = TASKFORGE_PRIO_NORMAL;
        return true;
    }
    if (strcasecmp(text, "LOW") == 0) {
        *out = TASKFORGE_PRIO_LOW;
        return true;
    }
    return false;
}

static bool id_exists(const workload_spec_t* spec, uint64_t id) {
    for (size_t i = 0; i < spec->count; i++) {
        if (spec->tasks[i].task_id == id) return true;
    }
    return false;
}

static bool ensure_capacity(workload_spec_t* spec, size_t additional) {
    if (additional > SIZE_MAX - spec->count) return false;
    size_t required = spec->count + additional;
    if (required <= spec->capacity) return true;

    size_t new_capacity = spec->capacity ? spec->capacity : 32;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(*spec->tasks)) return false;

    workload_task_desc_t* resized = realloc(spec->tasks, sizeof(*resized) * new_capacity);
    if (!resized) return false;
    spec->tasks = resized;
    spec->capacity = new_capacity;
    return true;
}

static uint64_t next_unique_id(const workload_spec_t* spec, uint64_t* next_id) {
    while (*next_id != 0 && id_exists(spec, *next_id)) {
        (*next_id)++;
    }
    if (*next_id == 0) return 0;
    return (*next_id)++;
}

static void* execute_parsed_task(void* arg) {
    workload_task_desc_t* desc = arg;
    if (!desc) return NULL;

    if (desc->sleep_ms > 0) {
        struct timespec delay = {
            .tv_sec = desc->sleep_ms / 1000U,
            .tv_nsec = (long)(desc->sleep_ms % 1000U) * 1000000L
        };
        while (nanosleep(&delay, &delay) != 0) {
            /* Retry if interrupted by a signal. */
        }
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

    workload_spec_t* spec = calloc(1, sizeof(*spec));
    if (!spec) return NULL;

    spec->capacity = 32;
    spec->tasks = malloc(sizeof(*spec->tasks) * spec->capacity);
    if (!spec->tasks) {
        free(spec);
        return NULL;
    }

    char* copy = strdup(text);
    if (!copy) {
        workload_spec_destroy(spec);
        return NULL;
    }

    uint64_t next_id = 1;
    size_t line_number = 0;
    char* saveptr = NULL;
    char* line = strtok_r(copy, "\r\n", &saveptr);

    while (line) {
        line_number++;
        while (isspace((unsigned char)*line)) line++;
        if (*line == '\0' || *line == '#') {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char cmd[32] = {0};
        if (sscanf(line, "%31s", cmd) != 1) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (strcmp(cmd, "TASK") == 0) {
            uint64_t tid = 0;
            char prio_str[16] = {0};
            uint32_t sleep_ms = 0;
            uint64_t iters = 0;
            int64_t payload = 0;
            char extra[2] = {0};

            int n = sscanf(line, "TASK %" SCNu64 " %15s %" SCNu32 " %" SCNu64 " %" SCNd64 " %1s",
                           &tid, prio_str, &sleep_ms, &iters, &payload, extra);
            if (n != 5 || tid == 0) {
                fprintf(stderr, "workload parser: invalid TASK on line %zu\n", line_number);
                goto fail;
            }

            taskforge_task_priority_t prio;
            if (!parse_priority(prio_str, &prio)) {
                fprintf(stderr, "workload parser: invalid priority on line %zu\n", line_number);
                goto fail;
            }
            if (id_exists(spec, tid)) {
                fprintf(stderr, "workload parser: duplicate task id %" PRIu64 " on line %zu\n", tid, line_number);
                goto fail;
            }
            if (!ensure_capacity(spec, 1)) goto fail;

            spec->tasks[spec->count++] = (workload_task_desc_t){
                .task_id = tid,
                .prio = prio,
                .sleep_ms = sleep_ms,
                .compute_iterations = iters,
                .payload = payload
            };
            if (next_id <= tid) next_id = (tid == UINT64_MAX) ? 0 : tid + 1;
        } else if (strcmp(cmd, "REPEAT") == 0) {
            size_t rep = 0;
            char prio_str[16] = {0};
            uint32_t sleep_ms = 0;
            uint64_t iters = 0;
            int64_t payload = 0;
            char extra[2] = {0};

            int n = sscanf(line, "REPEAT %zu TASK %15s %" SCNu32 " %" SCNu64 " %" SCNd64 " %1s",
                           &rep, prio_str, &sleep_ms, &iters, &payload, extra);
            if (n != 5 || rep == 0) {
                fprintf(stderr, "workload parser: invalid REPEAT on line %zu\n", line_number);
                goto fail;
            }

            taskforge_task_priority_t prio;
            if (!parse_priority(prio_str, &prio)) {
                fprintf(stderr, "workload parser: invalid priority on line %zu\n", line_number);
                goto fail;
            }
            if (!ensure_capacity(spec, rep)) goto fail;

            for (size_t r = 0; r < rep; r++) {
                uint64_t id = next_unique_id(spec, &next_id);
                if (id == 0) goto fail;
                spec->tasks[spec->count++] = (workload_task_desc_t){
                    .task_id = id,
                    .prio = prio,
                    .sleep_ms = sleep_ms,
                    .compute_iterations = iters,
                    .payload = payload
                };
            }
        } else {
            fprintf(stderr, "workload parser: unknown command '%s' on line %zu\n", cmd, line_number);
            goto fail;
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(copy);
    return spec;

fail:
    free(copy);
    workload_spec_destroy(spec);
    return NULL;
}

workload_spec_t* workload_spec_parse_file(const char* filepath) {
    if (!filepath) return NULL;
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long long)sz > SIZE_MAX - 1) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char* buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
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
        if (spec->count > SIZE_MAX / sizeof(*futures)) return -1;
        futures = calloc(spec->count, sizeof(*futures));
        if (!futures && spec->count > 0) return -1;
    }

    int status = 0;
    for (size_t i = 0; i < spec->count; i++) {
        workload_task_desc_t* item = malloc(sizeof(*item));
        if (!item) {
            status = -1;
            continue;
        }
        *item = spec->tasks[i];

        taskforge_future_t* fut = taskforge_submit_prio(pool, execute_parsed_task, item, item->prio);
        if (!fut) {
            free(item);
            status = -1;
        }

        if (wait_for_all) {
            futures[i] = fut;
        } else if (fut) {
            taskforge_future_release(fut);
        }
    }

    if (wait_for_all) {
        for (size_t i = 0; i < spec->count; i++) {
            if (!futures[i]) continue;
            void* res = NULL;
            taskforge_status_t wait_status = taskforge_future_wait(futures[i], &res);
            if (wait_status != TASKFORGE_OK && status == 0) status = (int)wait_status;
            taskforge_future_release(futures[i]);
        }
        free(futures);
    }

    return status;
}
