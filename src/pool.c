#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <string.h>
#include "taskforge/taskforge.h"
#include "taskforge/future.h"
#include "taskforge/queue.h"
#include "taskforge/work_stealing.h"
#include "taskforge/log.h"

static bool valid_priority(taskforge_task_priority_t prio) {
    return prio >= TASKFORGE_PRIO_LOW && prio <= TASKFORGE_PRIO_HIGH;
}


typedef struct {
    size_t id;
    pthread_t thread;
    taskforge_pool_t* pool;
    ws_deque_t deque;
    unsigned int rng_seed;
} worker_thread_t;

struct taskforge_pool {
    taskforge_pool_config_t config;
    worker_thread_t* workers;
    taskforge_queue_t* queue;
    taskforge_logger_t* logger;

    atomic_uint_fast64_t next_task_id;
    atomic_size_t active_workers;
    atomic_uint_fast64_t completed_tasks;
    atomic_uint_fast64_t rejected_tasks;
    atomic_uint_fast64_t stolen_tasks;

    atomic_bool shutdown_started;
    atomic_bool shutdown_complete;
    atomic_bool immediate_shutdown;
    pthread_mutex_t shutdown_mutex;
    pthread_cond_t shutdown_cond;
    size_t created_workers;
};

void taskforge_default_config(taskforge_pool_config_t* config) {
    if (!config) return;
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    config->num_workers = (nprocs > 0) ? (size_t)nprocs : 4;
    config->queue_capacity = 1024;
    config->enable_priority = false;
    config->enable_work_stealing = false;
    config->log_file_path = NULL;
}

static void execute_task_item(worker_thread_t* self, taskforge_task_t* task) {
    taskforge_pool_t* pool = self->pool;

    /* Check if task was cancelled before execution started */
    if (!future_mark_running(task->future)) {
        if (task->cleanup) task->cleanup(task->arg);
        future_release(task->future);
        return;
    }

    atomic_fetch_add(&pool->active_workers, 1);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    void* result = task->fn(task->arg);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double duration_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
                         (t_end.tv_nsec - t_start.tv_nsec) / 1000000.0;

    if (pool->logger) {
        taskforge_log_task(pool->logger, task->task_id, self->id,
                           TASKFORGE_FUTURE_COMPLETED, duration_ms, result, 0);
    }

    /*
     * Publish pool statistics before completing the future. A caller that
     * wakes from future_wait() must observe the task as completed in stats.
     */
    atomic_fetch_sub(&pool->active_workers, 1);
    atomic_fetch_add(&pool->completed_tasks, 1);
    future_complete(task->future, result);
}

static void* worker_loop(void* arg) {
    worker_thread_t* self = (worker_thread_t*)arg;
    taskforge_pool_t* pool = self->pool;

    while (1) {
        if (atomic_load(&pool->immediate_shutdown)) break;

        taskforge_task_t task;
        memset(&task, 0, sizeof(task));
        bool found_task = false;

        if (pool->config.enable_work_stealing) {
            /* 1. Try local deque (LIFO for cache locality) */
            if (ws_deque_pop_bottom(&self->deque, &task)) {
                found_task = true;
            }
            /* 2. Try stealing from other workers (FIFO) */
            else {
                size_t num = pool->config.num_workers;
                if (num > 1) {
                    size_t start_victim = (size_t)rand_r(&self->rng_seed) % num;
                    for (size_t i = 0; i < num; i++) {
                        size_t victim_idx = (start_victim + i) % num;
                        if (victim_idx == self->id) continue;
                        if (ws_deque_steal_top(&pool->workers[victim_idx].deque, &task)) {
                            found_task = true;
                            atomic_fetch_add(&pool->stolen_tasks, 1);
                            break;
                        }
                    }
                }
            }

            /* 3. If local and steal failed, try fetching a task + small prefetch batch from global queue */
            if (!found_task) {
                if (queue_try_pop(pool->queue, &task)) {
                    found_task = true;
                    /* Prefetch up to 3 tasks into local deque to amortize lock contention */
                    taskforge_task_t prefetch_task;
                    for (int b = 0; b < 3; b++) {
                        if (queue_try_pop(pool->queue, &prefetch_task)) {
                            if (!ws_deque_push_bottom(&self->deque, &prefetch_task)) {
                                /*
                                 * The task has already been removed from the global
                                 * queue, so it must never be converted into a
                                 * shutdown/failure result merely because the local
                                 * deque is full. Execute it directly instead.
                                 */
                                if (atomic_load(&pool->immediate_shutdown)) {
                                    if (prefetch_task.cleanup) prefetch_task.cleanup(prefetch_task.arg);
                                    future_fail(prefetch_task.future, TASKFORGE_ERR_SHUTDOWN);
                                } else {
                                    execute_task_item(self, &prefetch_task);
                                }
                            }
                        } else {
                            break;
                        }
                    }
                }
            }

            /* 4. If still nothing found, block on global queue */
            if (!found_task) {
                if (!queue_pop(pool->queue, &task)) {
                    /* Queue stopped and drained */
                    break;
                }
                found_task = true;
            }
        } else {
            /* Standard global queue mode: block until task available or queue stopped */
            if (!queue_pop(pool->queue, &task)) {
                break;
            }
            found_task = true;
        }

        if (!found_task) break;

        if (atomic_load(&pool->immediate_shutdown)) {
            if (task.cleanup) task.cleanup(task.arg);
            future_fail(task.future, TASKFORGE_ERR_SHUTDOWN);
            continue;
        }

        execute_task_item(self, &task);
    }

    /* Final drain for local deque if any tasks remain */
    if (pool->config.enable_work_stealing) {
        taskforge_task_t leftover;
        while (ws_deque_pop_bottom(&self->deque, &leftover)) {
            if (atomic_load(&pool->immediate_shutdown)) {
                if (leftover.cleanup) leftover.cleanup(leftover.arg);
                future_fail(leftover.future, TASKFORGE_ERR_SHUTDOWN);
            } else {
                execute_task_item(self, &leftover);
            }
        }
    }

    return NULL;
}

taskforge_pool_t* taskforge_pool_create(const taskforge_pool_config_t* config) {
    taskforge_pool_config_t cfg;
    if (config) {
        cfg = *config;
        if (cfg.num_workers == 0) {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            cfg.num_workers = (n > 0) ? (size_t)n : 4;
        }
        if (cfg.queue_capacity == 0) cfg.queue_capacity = 1024;
    } else {
        taskforge_default_config(&cfg);
    }

    taskforge_pool_t* pool = (taskforge_pool_t*)calloc(1, sizeof(taskforge_pool_t));
    if (!pool) return NULL;

    pool->config = cfg;
    atomic_init(&pool->next_task_id, 1);
    atomic_init(&pool->active_workers, 0);
    atomic_init(&pool->completed_tasks, 0);
    atomic_init(&pool->rejected_tasks, 0);
    atomic_init(&pool->stolen_tasks, 0);
    atomic_init(&pool->shutdown_started, false);
    atomic_init(&pool->shutdown_complete, false);
    atomic_init(&pool->immediate_shutdown, false);
    if (pthread_mutex_init(&pool->shutdown_mutex, NULL) != 0) { free(pool); return NULL; }
    if (pthread_cond_init(&pool->shutdown_cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->shutdown_mutex);
        free(pool);
        return NULL;
    }

    if (cfg.log_file_path) {
        pool->logger = taskforge_logger_create(cfg.log_file_path);
        if (!pool->logger) {
            pthread_cond_destroy(&pool->shutdown_cond);
            pthread_mutex_destroy(&pool->shutdown_mutex);
            free(pool);
            return NULL;
        }
    }

    pool->queue = queue_create(cfg.queue_capacity, cfg.enable_priority);
    if (!pool->queue) {
        if (pool->logger) taskforge_logger_destroy(pool->logger);
        pthread_cond_destroy(&pool->shutdown_cond);
        pthread_mutex_destroy(&pool->shutdown_mutex);
        free(pool);
        return NULL;
    }

    if (cfg.num_workers > SIZE_MAX / sizeof(worker_thread_t)) {
        queue_destroy(pool->queue);
        if (pool->logger) taskforge_logger_destroy(pool->logger);
        pthread_cond_destroy(&pool->shutdown_cond);
        pthread_mutex_destroy(&pool->shutdown_mutex);
        free(pool);
        return NULL;
    }

    pool->workers = (worker_thread_t*)calloc(cfg.num_workers, sizeof(worker_thread_t));
    if (!pool->workers) {
        queue_destroy(pool->queue);
        if (pool->logger) taskforge_logger_destroy(pool->logger);
        pthread_cond_destroy(&pool->shutdown_cond);
        pthread_mutex_destroy(&pool->shutdown_mutex);
        free(pool);
        return NULL;
    }

    size_t initialized_deques = 0;
    if (cfg.enable_work_stealing) {
        size_t per_worker_capacity = cfg.queue_capacity / cfg.num_workers;
        if (per_worker_capacity > SIZE_MAX - 64) {
            per_worker_capacity = SIZE_MAX;
        } else {
            per_worker_capacity += 64;
        }
        for (size_t i = 0; i < cfg.num_workers; i++) {
            pool->workers[i].id = i;
            pool->workers[i].pool = pool;
            pool->workers[i].rng_seed = (unsigned int)(time(NULL) ^ (uintptr_t)&pool->workers[i] ^ ((i + 1) * 7919));
            if (!ws_deque_init(&pool->workers[i].deque, per_worker_capacity)) {
                for (size_t j = 0; j < initialized_deques; j++) {
                    ws_deque_destroy(&pool->workers[j].deque);
                }
                free(pool->workers);
                queue_destroy(pool->queue);
                if (pool->logger) taskforge_logger_destroy(pool->logger);
                pthread_cond_destroy(&pool->shutdown_cond);
                pthread_mutex_destroy(&pool->shutdown_mutex);
                free(pool);
                return NULL;
            }
            initialized_deques++;
        }
    } else {
        for (size_t i = 0; i < cfg.num_workers; i++) {
            pool->workers[i].id = i;
            pool->workers[i].pool = pool;
            pool->workers[i].rng_seed = (unsigned int)(time(NULL) ^ (uintptr_t)&pool->workers[i] ^ ((i + 1) * 7919));
        }
    }

    for (size_t i = 0; i < cfg.num_workers; i++) {
        if (pthread_create(&pool->workers[i].thread, NULL, worker_loop, &pool->workers[i]) != 0) {
            taskforge_pool_shutdown(pool, false);
            taskforge_pool_destroy(pool);
            return NULL;
        }
        pool->created_workers++;
    }

    return pool;
}

taskforge_future_t* taskforge_submit_prio_with_cleanup(taskforge_pool_t* pool,
                                                       taskforge_task_fn fn,
                                                       void* arg,
                                                       taskforge_task_priority_t prio,
                                                       taskforge_task_cleanup_fn cleanup) {
    if (!pool || !fn || !valid_priority(prio) || atomic_load(&pool->shutdown_started)) {
        if (pool) atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    uint64_t id = atomic_fetch_add(&pool->next_task_id, 1);
    taskforge_future_t* future = future_create(id);
    if (!future) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    taskforge_task_t task;
    task.task_id = id;
    task.fn = fn;
    task.arg = arg;
    task.future = future;
    task.prio = prio;
    task.cleanup = cleanup;

    if (queue_push(pool->queue, &task) != TASKFORGE_OK) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        future_release(future);
        future_release(future);
        return NULL;
    }

    return future;
}

taskforge_future_t* taskforge_submit_prio(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg, taskforge_task_priority_t prio) {
    return taskforge_submit_prio_with_cleanup(pool, fn, arg, prio, NULL);
}

taskforge_future_t* taskforge_submit(taskforge_pool_t* pool, taskforge_task_fn fn, void* arg) {
    return taskforge_submit_prio(pool, fn, arg, TASKFORGE_PRIO_NORMAL);
}

taskforge_future_t* taskforge_try_submit_with_cleanup(taskforge_pool_t* pool,
                                                       taskforge_task_fn fn,
                                                       void* arg,
                                                       taskforge_task_priority_t prio,
                                                       taskforge_task_cleanup_fn cleanup) {
    if (!pool || !fn || !valid_priority(prio) || atomic_load(&pool->shutdown_started)) {
        if (pool) atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    uint64_t id = atomic_fetch_add(&pool->next_task_id, 1);
    taskforge_future_t* future = future_create(id);
    if (!future) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    taskforge_task_t task;
    task.task_id = id;
    task.fn = fn;
    task.arg = arg;
    task.future = future;
    task.prio = prio;
    task.cleanup = cleanup;

    taskforge_status_t status = queue_try_push(pool->queue, &task);
    if (status != TASKFORGE_OK) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        future_release(future);
        future_release(future);
        return NULL;
    }

    return future;
}

taskforge_future_t* taskforge_try_submit(taskforge_pool_t* pool,
                                         taskforge_task_fn fn,
                                         void* arg,
                                         taskforge_task_priority_t prio) {
    return taskforge_try_submit_with_cleanup(pool, fn, arg, prio, NULL);
}

taskforge_future_t* taskforge_submit_timeout_with_cleanup(taskforge_pool_t* pool,
                                             taskforge_task_fn fn,
                                             void* arg,
                                             taskforge_task_priority_t prio,
                                             uint32_t timeout_ms,
                                             taskforge_task_cleanup_fn cleanup) {
    if (!pool || !fn || !valid_priority(prio) || atomic_load(&pool->shutdown_started)) {
        if (pool) atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    uint64_t id = atomic_fetch_add(&pool->next_task_id, 1);
    taskforge_future_t* future = future_create(id);
    if (!future) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        return NULL;
    }

    taskforge_task_t task;
    task.task_id = id;
    task.fn = fn;
    task.arg = arg;
    task.future = future;
    task.prio = prio;
    task.cleanup = cleanup;

    taskforge_status_t status = queue_push_timeout(pool->queue, &task, timeout_ms);
    if (status != TASKFORGE_OK) {
        atomic_fetch_add(&pool->rejected_tasks, 1);
        if (cleanup) cleanup(arg);
        future_release(future);
        future_release(future);
        return NULL;
    }

    return future;
}

taskforge_future_t* taskforge_submit_timeout(taskforge_pool_t* pool,
                                             taskforge_task_fn fn,
                                             void* arg,
                                             taskforge_task_priority_t prio,
                                             uint32_t timeout_ms) {
    return taskforge_submit_timeout_with_cleanup(pool, fn, arg, prio, timeout_ms, NULL);
}

static bool caller_is_worker(taskforge_pool_t* pool) {
    if (!pool || !pool->workers) return false;
    pthread_t self = pthread_self();
    for (size_t i = 0; i < pool->created_workers; i++) {
        if (pthread_equal(self, pool->workers[i].thread)) return true;
    }
    return false;
}

int taskforge_pool_shutdown(taskforge_pool_t* pool, bool graceful) {
    if (!pool) return TASKFORGE_ERR_INVALID;
    if (caller_is_worker(pool)) return TASKFORGE_ERR_INVALID;

    bool expected = false;
    if (!atomic_compare_exchange_strong(&pool->shutdown_started, &expected, true)) {
        pthread_mutex_lock(&pool->shutdown_mutex);
        while (!atomic_load(&pool->shutdown_complete)) {
            pthread_cond_wait(&pool->shutdown_cond, &pool->shutdown_mutex);
        }
        pthread_mutex_unlock(&pool->shutdown_mutex);
        return TASKFORGE_OK;
    }

    if (!graceful) atomic_store(&pool->immediate_shutdown, true);
    queue_signal_shutdown(pool->queue, graceful);

    for (size_t i = 0; i < pool->created_workers; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }

    pthread_mutex_lock(&pool->shutdown_mutex);
    atomic_store(&pool->shutdown_complete, true);
    pthread_cond_broadcast(&pool->shutdown_cond);
    pthread_mutex_unlock(&pool->shutdown_mutex);
    return TASKFORGE_OK;
}

void taskforge_pool_destroy(taskforge_pool_t* pool) {
    if (!pool) return;
    if (caller_is_worker(pool)) return;

    if (!atomic_load(&pool->shutdown_complete)) {
        taskforge_pool_shutdown(pool, false);
    }

    if (pool->config.enable_work_stealing) {
        for (size_t i = 0; i < pool->config.num_workers; i++) {
            ws_deque_destroy(&pool->workers[i].deque);
        }
    }

    free(pool->workers);
    queue_destroy(pool->queue);

    if (pool->logger) {
        taskforge_logger_destroy(pool->logger);
    }

    pthread_cond_destroy(&pool->shutdown_cond);
    pthread_mutex_destroy(&pool->shutdown_mutex);
    free(pool);
}

taskforge_pool_stats_t taskforge_pool_get_stats(taskforge_pool_t* pool) {
    taskforge_pool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (!pool) return stats;

    stats.num_workers = pool->config.num_workers;
    stats.active_workers = atomic_load(&pool->active_workers);
    size_t queued = queue_size(pool->queue);
    if (pool->config.enable_work_stealing && pool->workers) {
        for (size_t i = 0; i < pool->config.num_workers; i++) {
            queued += ws_deque_size(&pool->workers[i].deque);
        }
    }
    stats.queued_tasks = queued;
    stats.completed_tasks = atomic_load(&pool->completed_tasks);
    stats.rejected_tasks = atomic_load(&pool->rejected_tasks);
    stats.stolen_tasks = atomic_load(&pool->stolen_tasks);
    return stats;
}
