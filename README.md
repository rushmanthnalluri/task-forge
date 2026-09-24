# TaskForge — Thread-Pool Task Engine
> **A high-performance C11 asynchronous task engine with bounded queues, composable futures, work-stealing per-worker deques, and robust graceful shutdown.**

Built for Linux / POSIX systems using C11, `pthreads`, and C11 atomics.

---

## 1. Highlights & Core Capabilities

* **Bounded Producer-Consumer Buffer (`taskforge_queue_t`)**:
  * Dual condition variables (`not_full`, `not_empty`) protecting a circular ring buffer under a single mutex.
  * Correct wait predicates (`while` loops, never `if`) to guarantee zero lost wakeups and immunity to spurious wakeups.
  * Bounded backpressure: producers block when full, or opt into non-blocking submissions (`taskforge_try_submit`) and timed waits (`taskforge_submit_timeout`).
* **Worker Pool & Thread Lifecycle (`taskforge_pool_t`)**:
  * Fixed or hardware-adaptive worker thread pool (`sysconf(_SC_NPROCESSORS_ONLN)`).
  * Dynamic load tracking: active workers, queued tasks, completed tasks, stolen tasks, and rejected tasks.
* **Composable Futures & Dual Reference Counting (`taskforge_future_t`)**:
  * Thread-safe completion channel with result slot (`void*`), error code, mutex, and condition variable.
  * Dual reference-counting invariant (Caller + Worker) guaranteeing **zero leaks**, **zero double frees**, and **zero use-after-free** even if caller waits, detaches, or releases early.
  * Timed waits (`taskforge_future_wait_timeout`) and queued task cancellation (`taskforge_future_cancel`).
* **Distinction Feature: Per-Worker Work-Stealing Deques**:
  * Each worker owns a double-ended queue (deque).
  * Worker pushes and pops from its own bottom (LIFO) for CPU cache locality.
  * Idle workers steal from other workers' top (FIFO) using `pthread_mutex_trylock`, eliminating global lock contention on multi-core systems (+19.5% throughput gain).
* **Multi-Level Priority Scheduling with Starvation Avoidance**:
  * Supports `HIGH`, `NORMAL`, and `LOW` priority queues.
  * Starvation prevention algorithm yields lower priorities after consecutive high-priority tasks.
* **Parallel Collection Mapping (`taskforge_map`)**:
  * High-level convenience API to map a function across an array of items in parallel and synchronize results.
* **Workload Lexer & Parser (`workload_spec_t`)**:
  * Parses declarative workload specification scripts (`workload.spec`) to drive reproducible benchmark scenarios.
* **Signal-Safe Shutdown (`SIGINT`) & On-Disk Persistence**:
  * Gracefully catches `SIGINT` (Ctrl+C), completes pending tasks, and joins workers without dropping tasks.
  * Thread-safe disk logger recording timestamps, worker IDs, and durations to disk (`taskforge_tasks.log`).

---

## 2. Architecture & Concurrency Model

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

## 3. Directory Layout

```
taskforge/
├── include/taskforge/
│   ├── taskforge.h           # Public C API (pool, future, submission, map, stats)
│   ├── future.h              # Future internals, states, and dual refcounting
│   ├── queue.h               # Bounded ring-buffer queue & priority management
│   ├── work_stealing.h       # Per-worker deques & theft protocol
│   ├── log.h                 # Thread-safe on-disk execution logger
│   └── parser.h              # Workload specification file parser
├── src/
│   ├── future.c              # Future state machine and refcount reclamation
│   ├── queue.c               # Bounded buffer, wait predicates, priority selection
│   ├── pool.c                # Worker loops, graceful drain, thread creation/joining
│   ├── work_stealing.c       # Deque push/pop/steal implementation
│   ├── map.c                 # taskforge_map parallel collection mapper
│   ├── log.c                 # File logging subsystem
│   └── parser.c              # Workload spec lexer and runner
├── cli/
│   └── taskforge_cli.c       # Interactive REPL console with SIGINT handler
├── tests/
│   ├── test_bounded_queue.c  # Producer/consumer blocking, capacity limit & backpressure
│   ├── test_futures.c        # Future wait, timed wait, and cancellation
│   ├── test_shutdown.c       # Graceful drain vs immediate kill verification
│   ├── test_priorities.c     # Multi-level priority order & starvation avoidance
│   ├── test_work_stealing.c  # Theft verification & work distribution
│   ├── test_map.c            # Parallel collection mapping test
│   ├── test_stress.c         # Multi-producer concurrency churn & race detection
│   └── test_million_soak.c   # 1,000,000 tasks soak test (acceptance verification)
├── benchmarks/
│   ├── bench_scaling.c       # Worker scaling curve & contention ceiling tool
│   └── bench_stealing_vs_global.c # Global queue vs work-stealing comparison
├── docs/
│   ├── DESIGN.md             # Formal system architecture & synchronization invariants
│   ├── SYLLABUS_MAPPING.md   # Week-by-week OS curriculum mapping (W1-W10 / W1-W12)
│   ├── BENCHMARK_REPORT.md   # Scalability report, contention ceiling analysis
│   └── scaling_curve.png     # Rendered scaling & speedup curve chart
├── scripts/
│   ├── run_valgrind.sh       # Automated memory leak verification
│   ├── run_tsan.sh           # Automated ThreadSanitizer race detector
│   └── plot_scaling.py       # Matplotlib scaling curve graph generator
├── workload.spec             # Sample workload specification script
└── Makefile                  # Multi-target build system
```

---

## 4. Building & Running

### Requirements
* Linux / WSL (Ubuntu 20.04+)
* GCC or Clang (C11 support)
* POSIX `pthreads`
* Python 3 + Matplotlib (optional, for plotting)
* Valgrind (optional, for memory profiling)

### Quick Commands

```bash
# Build static & shared libraries, CLI, tests, and benchmarks
make -j$(nproc) all

# Run full test suite (100% pass required)
make test

# Run the 1,000,000 tasks soak test
make test-million

# Run AddressSanitizer & UndefinedBehaviorSanitizer
make asan

# Run ThreadSanitizer (TSan race detector)
make tsan

# Run Valgrind leak checker (zero leaks verified)
make valgrind

# Run scalability & work-stealing benchmarks
make bench
```

---

## 5. Interactive CLI & REPL Driver

TaskForge includes a built-in CLI shell for interactive demonstrations and live vivas:

```bash
./bin/taskforge_cli
```

### Commands:
```
taskforge> status                       # Show worker count, queued, completed, stolen
taskforge> submit high 10 42            # Submit priority task (low|norm|high)
taskforge> map 1000                     # Run parallel map over 1,000 items
taskforge> run workload.spec            # Parse and execute workload script
taskforge> bench 8 50000                # Run instant benchmark with 8 workers
taskforge> shutdown graceful            # Gracefully drain pool
taskforge> exit                         # Clean exit
```
Pressing `Ctrl+C` (`SIGINT`) triggers signal-safe graceful teardown, drains all in-flight tasks, joins workers, and exits cleanly.

---

## 6. Acceptance & Evaluation Verification

| Specification Requirement | Verification Target | Result | Status |
|:---|:---|:---:|:---:|
| **1 Million Tasks Soak Test** | `make test-million` | **1,000,000 / 1,000,000 verified correct (812k tasks/s)** | **PASSED** |
| **Bounded Queue Blocking** | `test_bounded_queue` | Bounded capacity blocks producers; `TASKFORGE_ERR_FULL` / timeout | **PASSED** |
| **Future Protocol & Wait** | `test_futures` | Timed wait expires on slow tasks; normal wait retrieves results | **PASSED** |
| **Task Cancellation** | `test_futures` | Queued tasks cancel cleanly; workers skip; waiters get `ERR_CANCELLED` | **PASSED** |
| **Graceful Shutdown Drain** | `test_shutdown` | 100% in-flight tasks drain; new submissions rejected; 0 leaked | **PASSED** |
| **Zero Data Races (TSan)** | `make tsan` | 0 data races, 0 warnings across all concurrency tests | **PASSED** |
| **Zero Memory Leaks (Valgrind)**| `make valgrind` | 0 bytes leaked, 0 errors across entire heap lifecycle | **PASSED** |
| **ASan / UBSan Clean** | `make asan` | 0 buffer overflows, 0 use-after-free, 0 undefined behavior | **PASSED** |
| **Distinction: Work-Stealing** | `bench_stealing_vs_global` | Stealing yields **+19.5% throughput gain** under multi-core load | **PASSED** |
| **Scaling & Contention Curve** | `bench_scaling` | Contention ceiling identified; CSV & visual plot generated | **PASSED** |

---

## 7. C API Usage Example

```c
#include <stdio.h>
#include <taskforge/taskforge.h>

void* compute(void* arg) {
    int val = (int)(intptr_t)arg;
    return (void*)(intptr_t)(val * val);
}

int main(void) {
    taskforge_pool_config_t config;
    taskforge_default_config(&config);
    config.num_workers = 4;
    config.queue_capacity = 1024;
    config.enable_work_stealing = true;

    taskforge_pool_t* pool = taskforge_pool_create(&config);

    // 1. Submit work and receive future
    taskforge_future_t* fut = taskforge_submit(pool, compute, (void*)(intptr_t)9);

    // 2. Wait for result (or use taskforge_future_wait_timeout)
    void* result = NULL;
    taskforge_future_wait(fut, &result);
    printf("Result: %ld\n", (long)(intptr_t)result); // 81

    // 3. Release future handle
    taskforge_future_release(fut);

    // 4. Gracefully shutdown and join workers
    taskforge_pool_shutdown(pool, true);
    taskforge_pool_destroy(pool);
    return 0;
}
```
