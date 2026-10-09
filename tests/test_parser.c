#include <assert.h>
#include <stdio.h>
#include <string.h>
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

    /* scanf's unsigned conversions accept signs and silently leave an
     * overflowing value implementation-dependent; the grammar must reject
     * both cases for every numeric field. */
    assert(workload_spec_parse_string("TASK -1 HIGH 1 2 3\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH -1 2 3\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH 1 18446744073709551616 3\n") == NULL);
    assert(workload_spec_parse_string("REPEAT 18446744073709551616 TASK LOW 1 2 3\n") == NULL);

    /* Blank physical lines count toward diagnostics and do not disappear as
     * strtok would make them disappear.  More importantly, a large repeat
     * with no explicit ids should remain linear rather than rescanning all
     * generated ids for every item. */
    char repeat[128];
    strcpy(repeat, "\nREPEAT 10000 TASK LOW 0 0 0\n");
    workload_spec_t* repeated = workload_spec_parse_string(repeat);
    assert(repeated != NULL && repeated->count == 10000);
    assert(repeated->tasks[9999].task_id == 10000);
    workload_spec_destroy(repeated);

    const char* sparse_ids =
        "TASK 1000000000 HIGH 0 0 1\n"
        "REPEAT 5000 TASK LOW 0 0 2\n";
    workload_spec_t* sparse = workload_spec_parse_string(sparse_ids);
    assert(sparse != NULL && sparse->count == 5001);
    assert(sparse->tasks[1].task_id == 1000000001);
    assert(sparse->tasks[5000].task_id == 1000005000);
    workload_spec_destroy(sparse);

    const char* binary_path = "/tmp/taskforge-parser-nul.spec";
    FILE* binary = fopen(binary_path, "wb");
    assert(binary != NULL);
    const unsigned char binary_spec[] = "TASK 1 HIGH 0 0 1\0TASK 2 LOW 0 0 2\n";
    assert(fwrite(binary_spec, 1, sizeof(binary_spec) - 1, binary) == sizeof(binary_spec) - 1);
    assert(fclose(binary) == 0);
    assert(workload_spec_parse_file(binary_path) == NULL);
    remove(binary_path);

    printf("[PASS] test_parser completed successfully!\n\n");
    return 0;
}
