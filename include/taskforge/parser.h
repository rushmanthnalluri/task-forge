#ifndef TASKFORGE_PARSER_H
#define TASKFORGE_PARSER_H

#include "taskforge/taskforge.h"

typedef struct {
    uint64_t task_id;
    taskforge_task_priority_t prio;
    uint32_t sleep_ms;
    uint64_t compute_iterations;
    int64_t payload;
} workload_task_desc_t;

typedef struct {
    workload_task_desc_t* tasks;
    size_t count;
    size_t capacity;
} workload_spec_t;

workload_spec_t* workload_spec_parse_file(const char* filepath);
workload_spec_t* workload_spec_parse_string(const char* text);
void workload_spec_destroy(workload_spec_t* spec);
int workload_spec_execute(taskforge_pool_t* pool, const workload_spec_t* spec, bool wait_for_all);

#endif /* TASKFORGE_PARSER_H */
