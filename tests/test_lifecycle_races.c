#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include "taskforge/taskforge.h"

typedef struct {
    taskforge_pool_t* pool;
    atomic_bool stop;
    atomic_uint iterations;
    atomic_int unexpected_status;
} resize_race_t;

static void* resize_until_stopped(void* raw) {
    resize_race_t* race = (resize_race_t*)raw;
    size_t target = 1;
    while (!atomic_load(&race->stop)) {
        int status = taskforge_pool_resize(race->pool, target);
        if (status != TASKFORGE_OK && status != TASKFORGE_ERR_SHUTDOWN) {
            atomic_store(&race->unexpected_status, status);
        }
        atomic_fetch_add(&race->iterations, 1);
        target = target == 4 ? 1 : target + 1;
        /* Keep the resizer active while allowing the shutdown caller to run. */
        usleep(500);
    }
    return NULL;
}

int main(void) {
    alarm(15);

    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.num_workers = 2;
    config.queue_capacity = 8;
    config.enable_work_stealing = false;

    resize_race_t race = {0};
    atomic_init(&race.stop, false);
    atomic_init(&race.iterations, 0);
    atomic_init(&race.unexpected_status, TASKFORGE_OK);
    race.pool = taskforge_pool_create(&config);
    assert(race.pool != NULL);

    pthread_t resizer;
    assert(pthread_create(&resizer, NULL, resize_until_stopped, &race) == 0);
    while (atomic_load(&race.iterations) < 10) sched_yield();

    assert(taskforge_pool_shutdown(race.pool, true) == TASKFORGE_OK);
    atomic_store(&race.stop, true);
    assert(pthread_join(resizer, NULL) == 0);
    assert(atomic_load(&race.iterations) >= 10);
    assert(atomic_load(&race.unexpected_status) == TASKFORGE_OK);

    /* Destruction is deliberately after the concurrent API caller has joined. */
    taskforge_pool_destroy(race.pool);
    puts("[PASS] concurrent resize/shutdown serializes without invalid statuses");
    return 0;
}
