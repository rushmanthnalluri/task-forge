# TaskForge Design Document
**Architecture, Invariants, Synchronization Model, and Lifecycle Protocols**

---

## 1. System Overview

**TaskForge** is a thread-safe asynchronous task execution engine built from scratch in C11 using POSIX pthreads and C11 atomics. It implements the textbook bounded producer-consumer concurrency model extended with per-worker work-stealing deques, multi-level priority scheduling with starvation avoidance, non-blocking backpressure mechanisms, composable futures with timed synchronization, and atomic graceful/immediate shutdown protocols.

```
                           +-------------------------------------+
                           |            Caller / Main            |
                           +-------------------------------------+
                                       |              ^
                 taskforge_submit(...) |              | taskforge_future_wait(...)
                                       v              |
                       +-----------------------------------+
                       |      TaskForge Engine Core        |
                       +-----------------------------------+
                                  |              |
                     [Work-Stealing Mode]   [Global Queue Mode]
                                  |              |
           +----------------------+              +----------------------+
           |                      |                                     |
           v                      v                                     v
   +---------------+      +---------------+                   +--------------------+
   | Worker 0's    |      | Worker 1's    |                   | Bounded Buffer     |
   | Deque (LIFO)  |      | Deque (LIFO)  |                   | Ring-Queue         |
   +---------------+      +---------------+                   +--------------------+
         |   ^                  |   ^                         | Not-Full Condvar   |
   Owner |   | Steal (FIFO)     |   | Steal                   | Not-Empty Condvar  |
   Pop   |   +------------------+   |                         | Mutex              |
         v                          v                         +--------------------+
   +---------------+      +---------------+                             |
   | Worker Th 0   |      | Worker Th 1   | <--- Drains tasks ----------+
   +---------------+      +---------------+
```

---

## 2. Core Invariants

### 2.1 The Bounded Buffer (`taskforge_queue_t`)
The queue is protected by a single POSIX mutex (`mutex`) and coordinated via two condition variables (`not_empty`, `not_full`).

* **Wait Predicate Invariant**: All condition variable waits **MUST** be enclosed in a `while` loop, never an `if` statement.
  ```c
  while (queue->total_count >= queue->total_capacity && !queue->shutdown && !queue->draining) {
      pthread_cond_wait(&queue->not_full, &queue->mutex);
  }
  ```
  *Rationale*: POSIX allows spurious wakeups (`EINTR` or scheduler wakeups without explicit signal). Furthermore, under high multi-producer contention, another producer may claim the empty slot between signal delivery and mutex re-acquisition. A `while` loop guarantees correctness against lost and spurious wakeups.
* **Capacity Invariant**: `0 <= queue->total_count <= queue->total_capacity` holds at all times while the mutex is held or released.
* **FIFO & Priority Behavior**: Within a given priority tier, tasks are popped in FIFO order. Across tiers, the global queue selects `HIGH` before `NORMAL` and `LOW`, except after the starvation threshold yields a lower tier. When work stealing is enabled, tasks already prefetched into local deques are independent of later global-queue priority arrivals.

---

## 3. The Future Protocol & Memory Lifecycle

Each task submission returns an opaque handle `taskforge_future_t*`.

### 3.1 State Transitions
A future begins in `TASKFORGE_FUTURE_PENDING` and follows this state machine:

```
                  +---------------------------+
                  |  TASKFORGE_FUTURE_PENDING |
                  +---------------------------+
                     /                     \
    taskforge_future_cancel()       Worker picks up task
                   /                         \
                  v                           v
  +-----------------------------+   +---------------------------+
  |  TASKFORGE_FUTURE_CANCELLED |   |  TASKFORGE_FUTURE_RUNNING |
  +-----------------------------+   +---------------------------+
                                              /              \
                                  fn() returns        fn() errors
                                            /                  \
                                           v                    v
                          +--------------------+   +-------------------+
                          |  FUTURE_COMPLETED  |   |   FUTURE_FAILED   |
                          +--------------------+   +-------------------+
```

### 3.2 Dual Reference-Counting Invariant (Leak-Free Discipline)
A critical concurrency bug in thread pools is race conditions during future reclamation (e.g. caller destroys future before worker finishes, or caller exits without waiting).

* When created in `future_create()`, `atomic_init(&future->ref_count, 2)`.
  * **Reference 1**: Owned by the caller / submitter.
  * **Reference 2**: Owned by the worker executing the task.
* When the worker completes execution, it calls `future_complete()`, sets the result, broadcasts `future->cond`, and drops Reference 2 via `future_release()`.
* When the caller finishes inspecting or waiting on the future, it drops Reference 1 via `taskforge_future_release()`.
* The party that decrements `ref_count` to zero is solely responsible for destroying `pthread_mutex_t`, `pthread_cond_t`, and executing `free(future)`.
* This invariant prevents the worker-owned future from being reclaimed while the task is still executing. Sanitizer and leak results are environment-specific and should be reported from the run where they were executed.

---

## 4. Task Argument Ownership and Cleanup

Tasks submitted with `taskforge_submit_prio_with_cleanup()` may provide a cleanup callback for heap-owned arguments. The callback is invoked exactly when the task is accepted but never starts execution, including pending cancellation and immediate shutdown; if submission is rejected before acceptance, the callback is also invoked. Once the task function starts, ownership of the argument belongs to that function and the cleanup callback is not invoked. This separates executor-owned cancellation cleanup from task-owned execution cleanup.

## 5. Work-Stealing Per-Worker Deques

To conquer lock contention on multi-core architectures (e.g., 8-12 cores), TaskForge implements a work-stealing engine:
* **Worker Deque**: Each worker maintains a local double-ended queue.
* **Owner Operations (LIFO)**: The worker thread pushes new tasks to the bottom (`ws_deque_push_bottom`) and pops tasks from the bottom (`ws_deque_pop_bottom`). LIFO access maximizes CPU L1/L2 cache locality and hot data reuse.
* **Stealer Operations (FIFO)**: When a worker's deque and the global queue are empty, the idle worker becomes a stealer. It randomly picks a victim worker and steals from the top (`ws_deque_steal_top`). Stealing from the top takes the oldest, coarsest tasks, leaving finer-grained work for the victim.
* **Non-blocking Theft**: Stealers employ `pthread_mutex_trylock()`. If a victim worker is currently operating on its deque, the stealer does not block and immediately probes the next worker, preserving throughput.

---

## 6. Shutdown State Machine: Graceful Drain vs Immediate Kill

TaskForge provides two explicit shutdown semantics:

1. **Graceful Shutdown (`taskforge_pool_shutdown(pool, true)`)**:
   - Flips `queue->draining = true` and `pool->shutdown_started = true`.
   - All subsequent task submissions are rejected immediately with `NULL` / `TASKFORGE_ERR_SHUTDOWN`.
   - Workers continue popping until `queue->total_count == 0` and all local deques are empty.
   - All created worker threads are joined via `pthread_join()`.
   - Shutdown must be initiated by a non-worker caller.
   - Pool destruction must not run concurrently with shutdown or any other pool operation.
   - Result: 100% of submitted tasks finish execution; no tasks are dropped.

2. **Immediate Shutdown (`taskforge_pool_shutdown(pool, false)`)**:
   - Flips `queue->shutdown = true`.
   - Broadcasts to all condition variables (`not_empty`, `not_full`).
   - Workers stop taking new work. Tasks already executing are allowed to finish their current function. Remaining tasks in the global queue are marked `TASKFORGE_FUTURE_FAILED` with `TASKFORGE_ERR_SHUTDOWN`; tasks remaining in worker-local deques are failed rather than executed. Worker threads are then joined.

---

## 7. Backpressure & Non-Blocking Submissions

When consumers cannot keep up with producers, TaskForge provides 3 distinct submission options:
1. `taskforge_submit()`: Blocks producer thread on `not_full` until queue space opens up.
2. `taskforge_try_submit()`: Returns immediately with `NULL` (and increments `rejected_tasks` counter) if queue is full.
3. `taskforge_submit_timeout()`: Blocks on `not_full` for up to `timeout_ms` milliseconds using `pthread_cond_timedwait`. If no slot opens within the deadline, returns `NULL`.


## 8. Verification Scope

The repository tests distinguish implementation checks from environment-specific sanitizer and benchmark runs. Work-stealing tests verify deque ordering; priority tests verify starvation avoidance; shutdown tests verify graceful draining and immediate failure of queued work. Benchmark values are regenerated per host rather than treated as universal performance guarantees.


## 9. Failure-Path Contracts

Pool creation treats logger and work-stealing deque initialization failures as fatal and cleans up every resource initialized before the failure. Partial worker creation is tracked separately from configured worker count so all successfully created threads and all initialized deques are reclaimed.

Timed future and queue waits use condition variables configured for `CLOCK_MONOTONIC`, matching their monotonic deadlines. Workload parsing rejects malformed commands, unknown priorities, duplicate task IDs, and trailing tokens instead of silently accepting partial input.
