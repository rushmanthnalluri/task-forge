#ifndef TASKFORGE_H
#define TASKFORGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return and error status codes */
typedef enum {
    TASKFORGE_OK            =  0,
    TASKFORGE_ERR_INVALID   = -1,
    TASKFORGE_ERR_SHUTDOWN  = -2,
    TASKFORGE_ERR_FULL      = -3,
    TASKFORGE_ERR_TIMEOUT   = -4,
    TASKFORGE_ERR_CANCELLED = -5,
    TASKFORGE_ERR_NOMEM     = -6,
    TASKFORGE_ERR_FAILED    = -7
} taskforge_status_t;

/* Task priority levels */
typedef enum {
    TASKFORGE_PRIO_LOW    = 0,
    TASKFORGE_PRIO_NORMAL = 1,
    TASKFORGE_PRIO_HIGH   = 2,
    TASKFORGE_PRIO_COUNT  = 3
} taskforge_task_priority_t;

/* Future state enumeration */
typedef enum {
    TASKFORGE_FUTURE_PENDING   = 0,
    TASKFORGE_FUTURE_RUNNING   = 1,
    TASKFORGE_FUTURE_COMPLETED = 2,
    TASKFORGE_FUTURE_CANCELLED = 3,
    TASKFORGE_FUTURE_FAILED    = 4
} taskforge_future_state_t;

/* Task function signature */
typedef void* (*taskforge_task_fn)(void* arg);
typedef void (*taskforge_task_cleanup_fn)(void* arg);

/* Opaque types */
typedef struct taskforge_pool taskforge_pool_t;
typedef struct taskforge_future taskforge_future_t;

/* Pool configuration */
typedef struct {
    size_t num_workers;           /* Number of worker threads (0 = auto-detect CPU cores) */
    size_t queue_capacity;        /* Max tasks in bounded queue (0 = default 1024) */
    bool enable_priority;         /* Enable multi-level priority queues */
    bool enable_work_stealing;    /* Enable per-worker deques and work-stealing */
    const char* log_file_path;    /* Optional file path for task execution persistence */
} taskforge_pool_config_t;

/* Pool statistics */
typedef struct {
    size_t num_workers;
    size_t active_workers;
    size_t queued_tasks;
    size_t completed_tasks;
    size_t rejected_tasks;
    size_t stolen_tasks;
} taskforge_pool_stats_t;

/* Default pool configuration helper */
void taskforge_default_config(taskforge_pool_config_t* config);

/* Pool lifecycle */
taskforge_pool_t* taskforge_pool_create(const taskforge_pool_config_t* config);
int taskforge_pool_shutdown(taskforge_pool_t* pool, bool graceful);
void taskforge_pool_destroy(taskforge_pool_t* pool);
taskforge_pool_stats_t taskforge_pool_get_stats(taskforge_pool_t* pool);

/* Task submission */
taskforge_future_t* taskforge_submit(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg);
taskforge_future_t* taskforge_submit_prio(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg, taskforge_task_priority_t prio);
taskforge_future_t* taskforge_submit_prio_with_cleanup(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg, taskforge_task_priority_t prio, taskforge_task_cleanup_fn cleanup);
taskforge_future_t* taskforge_submit_timeout(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg, taskforge_task_priority_t prio, uint32_t timeout_ms);
taskforge_future_t* taskforge_try_submit(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg, taskforge_task_priority_t prio);

/* Future operations */
taskforge_status_t taskforge_future_wait(taskforge_future_t* future, void** out_result);
taskforge_status_t taskforge_future_wait_timeout(taskforge_future_t* future, uint32_t timeout_ms, void** out_result);
taskforge_future_state_t taskforge_future_get_state(taskforge_future_t* future);
int taskforge_future_get_error(taskforge_future_t* future);
bool taskforge_future_cancel(taskforge_future_t* future);
void taskforge_future_release(taskforge_future_t* future);

/* Map convenience API: process collection in parallel and wait for all */
taskforge_status_t taskforge_map(taskforge_pool_t* pool,
                                 taskforge_task_fn map_fn,
                                 void** items,
                                 size_t count,
                                 void** results);

#ifdef __cplusplus
}
#endif

#endif /* TASKFORGE_H */
