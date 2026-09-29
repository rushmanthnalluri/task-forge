#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taskforge/taskforge.h"

static void* log_task(void* arg) {
    return arg;
}

int main(void) {
    printf("[TEST] Running test_logging...\n");

    const char* path = "taskforge_test.log";
    remove(path);

    taskforge_pool_config_t cfg;
    taskforge_default_config(&cfg);
    cfg.num_workers = 1;
    cfg.queue_capacity = 8;
    cfg.log_file_path = path;

    taskforge_pool_t* pool = taskforge_pool_create(&cfg);
    assert(pool != NULL);

    taskforge_future_t* fut = taskforge_submit(pool, log_task, (void*)(intptr_t)123);
    assert(fut != NULL);
    assert(taskforge_future_wait(fut, NULL) == TASKFORGE_OK);
    taskforge_future_release(fut);

    assert(taskforge_pool_shutdown(pool, true) == TASKFORGE_OK);
    taskforge_pool_destroy(pool);

    FILE* f = fopen(path, "r");
    assert(f != NULL);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(path);

    assert(n > 0);
    assert(strstr(buf, "COMPLETED") != NULL);
    assert(strstr(buf, "WORKER 0") != NULL);

    printf("[PASS] test_logging completed successfully!\n\n");
    return 0;
}
