#include <assert.h>
#include <stdio.h>
#include "taskforge/parser.h"

int main(void) {
    printf("[TEST] Running test_parser...\n");

    const char* text =
        "# comment\n"
        "TASK 7 HIGH 5 100 42\n"
        "REPEAT 3 TASK LOW 1 20 9\n"
        "TASK 8 NORMAL 0 0 12\n";

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
    assert(workload_spec_parse_string(NULL) == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH 1 2\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 URGENT 1 2 3\n") == NULL);
    assert(workload_spec_parse_string("UNKNOWN 1 2 3\n") == NULL);
    assert(workload_spec_parse_string("TASK 1 HIGH 1 2 3\nTASK 1 LOW 1 2 3\n") == NULL);

    printf("[PASS] test_parser completed successfully!\n\n");
    return 0;
}
