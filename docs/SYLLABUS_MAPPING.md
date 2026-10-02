# TaskForge — OS Syllabus & Rubric Mapping

This document provides a detailed mapping between the Operating Systems course curriculum (both the 10-Week Continuous Evaluation Rubric and 12-Week Syllabus) and the architectural implementations in **TaskForge**.

---

## 1. Rubric & Week-by-Week Mapping

| Week | Course Topic & Focus | What TaskForge Implements | Key Code Artifacts | Weight |
|:---:|:---|:---|:---|:---:|
| **W1** | **Environment & Project Setup**<br>Syscall boundary · POSIX pthreads · REPL & repo setup | Clean C11 project directory structure, Makefile with multiple targets, interactive CLI REPL driver with commands (`status`, `submit`, `map`, `run`, `bench`, `shutdown`). | `cli/taskforge_cli.c`, `Makefile`, `include/taskforge/taskforge.h` | 5% |
| **W2** | **Problem Understanding & Memory Model**<br>Stack, heap, linker · dynamic memory ownership | Formal design documentation of queue invariants, pointer ownership, and dual reference-counting memory lifecycle preventing use-after-free and double-frees. | `docs/DESIGN.md`, `src/future.c` (`future_create`, `future_retain`, `future_release`) | 8% |
| **W3** | **Basic Prototype & Workload Parsing**<br>Task descriptor table · Lexing & parsing · console I/O | Task descriptor structure (`workload_task_desc_t`), lexer/parser for workload specification scripts with priority and repeat commands, and runner pipeline. | `include/taskforge/parser.h`, `src/parser.c`, `workload.spec` | 10% |
| **W4** | **Core Module I — Dispatch & Scheduling**<br>Worker thread pool · CPU scheduling concepts | Fixed worker pool spawning POSIX worker threads (`pthread_create`), cooperative task dispatching, and dynamic worker load tracking. | `src/pool.c` (`worker_loop`, `taskforge_pool_create`, `taskforge_pool_get_stats`) | 10% |
| **W5** | **Core Module II — Futures & Results**<br>ELF loading/function pointers · exit codes & status | Opaque `taskforge_future_t` with mutex and condvar, task functions as generic `void* (*)(void*)`, return values, failure codes, and timed waits (`taskforge_future_wait_timeout`). | `include/taskforge/future.h`, `src/future.c` | 12% |
| **W6** | **Integration & Signals**<br>Signals · asynchronous control · graceful teardown | Asynchronous signal handler for `SIGINT` (Ctrl+C), atomic shutdown flags, clean worker thread joining (`pthread_join`), and module integration into static/shared libraries. | `cli/taskforge_cli.c` (`sigint_handler`), `src/pool.c` (`taskforge_pool_shutdown`) | 12% |
| **W7** | **Concurrency & Sleep/Wakeup**<br>Producer/consumer · Condition variables (`not_full`, `not_empty`) | Complete bounded ring buffer with dual condition variables under one mutex. Enforces `while` loop predicates against spurious wakeups and lost wakeups. | `include/taskforge/queue.h`, `src/queue.c` (`queue_push`, `queue_pop`, `queue_try_push`, `queue_push_timeout`) | 10% |
| **W8** | **Testing & Debugging**<br>Leak-free memory discipline · Valgrind · ASan · TSan | Comprehensive automated test battery, AddressSanitizer/UBSan target, Valgrind leak check scripts, and multi-producer stress test. | `tests/test_*.c`, `scripts/run_valgrind.sh`, `scripts/run_tsan.sh` | 10% |
| **W9** | **Optimization & Persistence**<br>Files · file descriptors · on-disk task logging | Thread-safe disk logger recording timestamps, worker IDs, task execution durations, and logged execution states to disk for offline auditing. | `include/taskforge/log.h`, `src/log.c`, `taskforge_tasks.log` | 8% |
| **W10** | **Final Demo, Scaling & Viva**<br>Scaling curves · contention ceiling · 1,000,000 tasks soak test | Soak test validating 1,000,000 tasks across N producers and M workers with 100% correctness. Automated scalability benchmark with work-stealing vs global queue. | `benchmarks/bench_scaling.c`, `benchmarks/bench_stealing_vs_global.c`, `tests/test_million_soak.c` | 15% |

---

## 2. Distinction-Oriented Features

TaskForge includes several features beyond the core specification:

1. **Per-Worker Work-Stealing Deques**:
   - Each worker has its own local deque (`ws_deque_t`).
   - Push and pop at bottom (LIFO) ensures CPU cache locality.
   - Steal from top (FIFO) balances load dynamically under skewed or bursty workloads.
   - Non-blocking theft using `pthread_mutex_trylock` can reduce contention on the global queue for workloads that benefit from local execution.
2. **Prioritization with Starvation Avoidance**:
   - Multi-level priority queues (`HIGH`, `NORMAL`, `LOW`).
   - Built-in starvation threshold counter provides a bounded high-priority streak in the global queue; local work-stealing queues can still affect global ordering.
3. **Task Cancellation**:
   - Cancellation of pending tasks prior to worker execution via `taskforge_future_cancel()`.
   - Worker skips execution cleanly; waiters receive `TASKFORGE_ERR_CANCELLED`.
4. **Convenience Map API**:
   - `taskforge_map()` parallelizes arbitrary array workloads and synchronizes completions.
