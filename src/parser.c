#include "taskforge/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>

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

static bool parse_u64(const char* text, uint64_t* out) {
    if (!text || !*text || *text == '-') return false;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++)
        if (!isdigit(*p)) return false;
    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value > UINT64_MAX) return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_u32(const char* text, uint32_t* out) {
    uint64_t value;
    if (!parse_u64(text, &value) || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_size(const char* text, size_t* out) {
    uint64_t value;
    if (!parse_u64(text, &value) || value > SIZE_MAX) return false;
    *out = (size_t)value;
    return true;
}

static bool parse_i64(const char* text, int64_t* out) {
    if (!text || !*text) return false;
    const char* p = text;
    if (*p == '-') p++;
    if (!*p) return false;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    errno = 0;
    char* end = NULL;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return false;
    *out = (int64_t)value;
    return true;
}

static size_t fields(char* line, char** out, size_t limit) {
    size_t count = 0;
    while (*line) {
        while (isspace((unsigned char)*line)) line++;
        if (!*line) break;
        if (count == limit) return limit + 1;
        out[count++] = line;
        while (*line && !isspace((unsigned char)*line)) line++;
        if (*line) *line++ = '\0';
    }
    return count;
}

typedef struct {
    uint64_t* slots;
    size_t capacity;
    size_t count;
} id_set_t;

static size_t id_hash(uint64_t id) {
    id ^= id >> 30;
    id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27;
    id *= UINT64_C(0x94d049bb133111eb);
    id ^= id >> 31;
    return (size_t)id;
}

static bool id_set_contains(const id_set_t* set, uint64_t id) {
    if (!set || !set->slots || id == 0) return false;
    size_t mask = set->capacity - 1;
    size_t index = id_hash(id) & mask;
    while (set->slots[index] != 0) {
        if (set->slots[index] == id) return true;
        index = (index + 1) & mask;
    }
    return false;
}

static bool id_set_rehash(id_set_t* set, size_t capacity) {
    if (capacity < 16 || (capacity & (capacity - 1)) != 0 ||
        capacity > SIZE_MAX / sizeof(*set->slots)) return false;
    uint64_t* slots = calloc(capacity, sizeof(*slots));
    if (!slots) return false;
    if (set->slots) {
        for (size_t i = 0; i < set->capacity; i++) {
            uint64_t id = set->slots[i];
            if (id != 0) {
                size_t index = id_hash(id) & (capacity - 1);
                while (slots[index] != 0) index = (index + 1) & (capacity - 1);
                slots[index] = id;
            }
        }
        free(set->slots);
    }
    set->slots = slots;
    set->capacity = capacity;
    return true;
}

static bool id_set_insert(id_set_t* set, uint64_t id) {
    if (!set || id == 0 || id_set_contains(set, id)) return false;
    if (!set->capacity && !id_set_rehash(set, 16)) return false;
    if (set->count >= set->capacity - set->capacity / 3) {
        if (set->capacity > SIZE_MAX / 2 || !id_set_rehash(set, set->capacity * 2)) return false;
    }
    size_t index = id_hash(id) & (set->capacity - 1);
    while (set->slots[index] != 0) index = (index + 1) & (set->capacity - 1);
    set->slots[index] = id;
    set->count++;
    return true;
}

static void id_set_destroy(id_set_t* set) {
    if (!set) return;
    free(set->slots);
    memset(set, 0, sizeof(*set));
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

static uint64_t next_unique_id(const id_set_t* ids, uint64_t* next_id) {
    while (*next_id != 0 && id_set_contains(ids, *next_id)) {
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
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            /* Retry if interrupted by a signal. */
        }
    }

    /* Use unsigned arithmetic for the intentionally wrapping LCG step.
     * Signed overflow here would be undefined behavior under C11. */
    uint64_t val = (uint64_t)desc->payload;
    for (uint64_t i = 0; i < desc->compute_iterations; i++) {
        val = (val * UINT64_C(1103515245) + UINT64_C(12345)) & UINT64_C(0x7fffffff);
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
    id_set_t ids = {0};
    size_t line_number = 0;
    char* line = copy;

    while (line) {
        line_number++;
        char* next_line = strpbrk(line, "\r\n");
        if (next_line) {
            char terminator = *next_line;
            *next_line++ = '\0';
            if (terminator == '\r' && *next_line == '\n') next_line++;
        }
        while (isspace((unsigned char)*line)) line++;
        if (*line == '\0' || *line == '#') {
            line = next_line;
            continue;
        }

        /* Strip an inline comment. Workload syntax has no quoted fields, so
         * the first '#' always starts a comment once the line is non-empty. */
        char* comment = strchr(line, '#');
        if (comment && (comment == line || isspace((unsigned char)comment[-1]))) {
            *comment = '\0';
            while (comment > line && isspace((unsigned char)comment[-1])) comment--;
            *comment = '\0';
        }

        char* tok[8] = {0};
        size_t ntok = fields(line, tok, 7);
        if (ntok == 0) {
            line = next_line;
            continue;
        }
        char* cmd = tok[0];

        if (strcmp(cmd, "TASK") == 0) {
            uint64_t tid = 0;
            uint32_t sleep_ms = 0;
            uint64_t iters = 0;
            int64_t payload = 0;
            if (ntok != 6 || !parse_u64(tok[1], &tid) || !parse_u32(tok[3], &sleep_ms) ||
                !parse_u64(tok[4], &iters) || !parse_i64(tok[5], &payload) || tid == 0) {
                fprintf(stderr, "workload parser: invalid TASK on line %zu\n", line_number);
                goto fail;
            }

            taskforge_task_priority_t prio;
            if (!parse_priority(tok[2], &prio)) {
                fprintf(stderr, "workload parser: invalid priority on line %zu\n", line_number);
                goto fail;
            }
            if (id_set_contains(&ids, tid)) {
                fprintf(stderr, "workload parser: duplicate task id %" PRIu64 " on line %zu\n", tid, line_number);
                goto fail;
            }
            if (!ensure_capacity(spec, 1)) goto fail;

            if (!id_set_insert(&ids, tid)) goto fail;
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
            uint32_t sleep_ms = 0;
            uint64_t iters = 0;
            int64_t payload = 0;
            if (ntok != 7 || strcmp(tok[2], "TASK") != 0 || !parse_size(tok[1], &rep) ||
                !parse_u32(tok[4], &sleep_ms) || !parse_u64(tok[5], &iters) ||
                !parse_i64(tok[6], &payload) || rep == 0) {
                fprintf(stderr, "workload parser: invalid REPEAT on line %zu\n", line_number);
                goto fail;
            }

            taskforge_task_priority_t prio;
            if (!parse_priority(tok[3], &prio)) {
                fprintf(stderr, "workload parser: invalid priority on line %zu\n", line_number);
                goto fail;
            }
            if (!ensure_capacity(spec, rep)) goto fail;

            for (size_t r = 0; r < rep; r++) {
                uint64_t id = next_unique_id(&ids, &next_id);
                if (id == 0) goto fail;
                if (!id_set_insert(&ids, id)) goto fail;
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

        line = next_line;
    }

    free(copy);
    id_set_destroy(&ids);
    return spec;

fail:
    free(copy);
    id_set_destroy(&ids);
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
    if (ferror(f) || rd != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    /* The string parser deliberately accepts a C string.  Reject binary
     * input here rather than silently parsing only the prefix before NUL. */
    if (memchr(buf, '\0', rd) != NULL) {
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

        taskforge_future_t* fut = taskforge_submit_prio_with_cleanup(pool, execute_parsed_task, item, item->prio, free);
        if (!fut) {
            /* submit_with_cleanup owns cleanup on post-creation rejection. */
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
            if (wait_status != TASKFORGE_OK && status == 0) {
                if (wait_status == TASKFORGE_ERR_FAILED) {
                    int error_code = taskforge_future_get_error(futures[i]);
                    status = error_code != 0 ? error_code : (int)wait_status;
                } else {
                    status = (int)wait_status;
                }
            }
            taskforge_future_release(futures[i]);
        }
        free(futures);
    }

    return status;
}
