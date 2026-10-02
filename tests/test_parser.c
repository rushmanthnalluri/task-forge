#include <assert.h>
#include <stdio.h>
#include "taskforge/parser.h"

int main(void) {
    printf("[TEST] Running test_parser...\n");

    const char* text =
        "# comment\n"
        "TASK 7 HIGH 5 100 42\n"
        "REPEAT 3 TASK LOW 1 20 9\n"
        "TASK 11 NORMAL 0 0 12\n";

    workload_spec_t* spec = workload_spec_parse_string(text);
    assert(spec != NULL);
    assert(spec->count == 5);
    assert(spec->tasks[0].task_id == 7);
    assert(spec->tasks[0].prio == TASKFORGE_PRIO_HIGH);
    assert(spec->tasks[0].sleep_ms == 5);
    assert(spec->tasks[0].compute_iterations == 100);
    assert(spec->tasks[0].payload == 42);
    assert(spec->tasks[1].prio == TASKFORGE_PRIO_LOW);
    assert(spec->tasks[4].task_id == 11);
    assert(spec->tasks[4].prio == TASKFORGE_PRIO_NORMAL);

    workload_spec_destroy(spec);

    /* Exercise the executor with a payload whose signed LCG multiplication
     * would overflow int64_t if the implementation used signed arithmetic. */
    const char* overflow_safe_text = "TASK 1 HIGH 0 1 9223372036854775807\n";
    workload_spec_t* overflow_spec = workload_spec_parse_string(overflow_safe_text);
    assert(overflow_spec != NULL);

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1;
    cfg.queue_capacity = 4;
    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);
    assert(workload_spec_execute(pool, overflow_spec, true) == 0);
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);
    workload_spec_destroy(overflow_spec);

    assert(workload_spec_parse_string(NULL) == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH 1 2\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 URGENT 1 2 3\n") == NULL);
    assert(workload_spec_parse_string("UNKNOWN 1 2 3\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH 1 2 3\nTASK 1 LOW 1 2 3\n") == NULL);

    printf("[PASS] test_parser completed successfully!\n\n");
    return 0;
}
