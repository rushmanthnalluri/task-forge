# TaskForge audit report

**Mission deadline:** 2026-10-10 12:50 IST (UTC+05:30)  
**Latest verified CI completion:** 2026-10-10T01:46:17Z (2026-10-10 07:16:17 IST)  
**Baseline branch:** `main`  
**Latest verified main commit:** `a4d0d38bc12176df706af0fb7d9c2d8dfaa536a6`  
**Coverage status:** In progress; see subsystem ledger below.

## Scope and method

The GitHub repository connector was used to inspect the complete recursive tracked-file tree (44 tracked files; tree response was not truncated), relevant source files, public headers, tests, Makefile, CI workflow, README, and design document. Repository access and push permission were confirmed. The available environment does not expose a local clone or local shell/Git working tree, so local `git status`, local runtime inventory, and local test execution are unavailable. Verification must use observed GitHub Actions results.

No dependency manifest or third-party package lockfile is present in the tracked tree. The codebase is a C11/POSIX pthreads library with Make-based builds. CI has a single GitHub Actions workflow.

## Findings

### TF-RESIZE-001 — Worker callback can deadlock while resizing down
- **Component:** `src/pool.c`, `taskforge_pool_resize`, `taskforge_pool_worker_count`
- **Severity/confidence:** P1 / high
- **Evidence:** Shrinking held `resize_mutex` while joining retiring workers. `taskforge_pool_worker_count()` also acquired this mutex. If a callback on a retiring worker queried the worker count, the resizer waited for the callback while the callback waited for the resizer's mutex.
- **Impact:** Resize can hang indefinitely for valid callback behavior.
- **Remediation:** Make worker-count reads atomic and keep worker-local deque mutexes alive across resize cycles; destroy them only during pool destruction after workers are joined.
- **Regression test:** `tests/test_resize_stress.c` now exercises worker callbacks querying count/stats while a shrink is joining.
- **Status:** Implemented on combined PR #5; CI run [37966854175](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966854175) passed all five gates on head `3e3ebe6918d92ced72b0ea7914523bfd508b651c`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.
- **Commit:** `ce43a233a47d3ae9acf71b531b5257129764cb09` (source fix).

### TF-RESIZE-002 — Statistics race with worker resize/deque destruction
- **Component:** `src/pool.c`, `taskforge_pool_get_stats`
- **Severity/confidence:** P1 / high
- **Evidence:** Stats read mutable `config.num_workers` while resize wrote it, and traversed local deques that resize destroyed after joining workers. A concurrent stats call could race with the configuration write or touch a destroyed deque mutex.
- **Impact:** Data race, undefined behavior, or a hang/crash during concurrent stats and resize.
- **Remediation:** Derive worker count from the atomic operational-worker count. Retain initialized deques through shrink/re-growth and destroy them only after shutdown at pool destruction.
- **Regression test:** Same resize/stats regression in `tests/test_resize_stress.c`.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.
- **Commit:** `ce43a233a47d3ae9acf71b531b5257129764cb09`.

### TF-IPC-001 — IPC response framing, protocol checks, and read deadlines

- **Severity/confidence:** P2 / high.
- **Evidence:** The baseline client read response bodies with a newline reader even though the response header carried a byte length; the server parsed but did not enforce the protocol version.
- **Impact:** Multiline handler results were truncated/misframed and incompatible protocol versions could reach dispatch.
- **Remediation:** Read the advertised body length exactly, validate the trailing delimiter and response header, enforce protocol version, validate handler names/entries, and use a shared monotonic deadline across the response header/body.
- **Regression tests:** Multiline result round-trip; unsupported version against a registered handler; slow-trickle body must time out against one total deadline.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-IPC-002 — Unsafe socket-path deletion

- **Severity/confidence:** P1 / high.
- **Evidence:** Baseline startup unconditionally unlinked the requested path before bind and also unlinked after bind/listen failure. The stop function unlinked any path.
- **Impact:** A caller-supplied regular file could be deleted, or a path not created by this server could be removed.
- **Remediation:** Reject existing paths, do not unlink on bind failure, record the bound socket's device/inode and only clean up that same socket, and reject non-socket paths in the cleanup API.
- **Regression test:** A pre-existing regular file remains byte-for-byte unchanged after attempted server start/stop.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-IPC-003 — Ambiguous request text was not rejected

- **Severity/confidence:** P2 / high.
- **Evidence:** The v1 request format uses a newline-terminated text line; NUL, CR, or LF inside the argument cannot be represented unambiguously.
- **Impact:** Such input could be truncated or interpreted as protocol framing.
- **Remediation:** Explicitly reject NUL/CR/LF request arguments before connecting; keep the supported v1 argument contract text-only. Response bodies are length-framed and can contain newlines.
- **Regression test:** Client calls with NUL/CR/LF arguments fail before connecting.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-IPC-004 — Handler failure could be reported as success

- **Severity/confidence:** P2 / high.
- **Evidence:** A handler returning failure while leaving its error output zero could produce a failure response with error code zero, which the client returned as success.
- **Impact:** Callers could mistake failed work for successful work.
- **Remediation:** Normalize failure-with-zero-error to `TASKFORGE_ERR_FAILED` on both server and client paths.
- **Regression test:** A handler that fails without setting an error code must return `TASKFORGE_ERR_FAILED`.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-IPC-005 — Server stop API does not stop the listener

- **Severity/confidence:** P2 (API contract clarity) / high.
- **Evidence:** `taskforge_ipc_server_stop` only unlinks the socket pathname; it does not wake the server's blocking `accept()` loop.
- **Impact:** Callers could believe a listening server process had terminated when it was merely unlinked from its pathname.
- **Remediation/status:** Public header and README now document that it only unlinks a socket path and does not terminate the server. A future true-stop API needs an explicit server handle or authenticated control channel.
### TF-QUEUE-001 — Producer ticket cancellation can fail open into a permanent stall
- **Component:** `src/queue.c`, `cancel_ticket`, `abandon_ticket`
- **Severity/confidence:** P2 / medium-high
- **Evidence:** Cancellation bookkeeping grows a heap array. If `realloc` fails, `cancel_ticket` returns false, but `abandon_ticket` ignores the result. The missing ticket can prevent `producer_turn` from advancing, leaving later producers blocked even after capacity becomes available.
- **Impact:** Under memory pressure, bounded-queue producers may stall indefinitely.
- **Remediation:** Removed the heap-backed producer-ticket cancellation ledger. Producers now wait on queue capacity and shutdown predicates directly, so cancellation cannot lose a ticket due to `realloc` failure.
- **Regression test:** Added 24 concurrent timed-out producers and verified the queue still accepts and drains later work.
- **Trade-off:** producer wake-up order is scheduler-dependent; strict FIFO fairness is not promised. This is now stated in `include/taskforge/queue.h`.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-MAP-001 — Timed map per-item reports do not describe all canceled items
- **Component:** `src/map.c`, `taskforge_map_timeout_report`
- **Severity/confidence:** P2 / medium
- **Evidence:** On timeout at index `i`, later futures are canceled and released, but their report entries retain the initial generic failure values rather than the resulting canceled state/status.
- **Impact:** Callers cannot reliably distinguish timed-out, canceled, and failed items in the report.
- **Remediation:** When the shared deadline expires, cancel the current item if it is still pending; for later items, record `CANCELLED` when cancellation succeeds or perform a zero-time terminal-state check so completed, failed, and still-running items are reported accurately. Running tasks are not forcibly interrupted.
- **Regression test:** A one-worker pool runs a slow first item and leaves the second queued; the report must mark the first `TIMEOUT` and the second `CANCELLED`.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-MAP-002 — Submission failure leaves earlier map report entries at their default failure state

- **Component:** `src/map.c`, submission-failure cleanup in `map_impl`
- **Severity/confidence:** P2 / high
- **Evidence:** When submission of item `i` failed, the code waited for earlier futures but did not copy their terminal statuses/results into the optional per-item report, leaving successful items marked failed.
- **Impact:** Callers received inaccurate per-item diagnostics and could lose successful result pointers in the report.
- **Remediation:** Capture each earlier future's result/status/error while draining submitted work and update the report.
- **Regression test:** A one-worker, capacity-one pool runs item 1, queues item 2, then concurrent immediate shutdown rejects item 3; the report must retain item 1's success and item 2's shutdown failure.
- **Status:** Implemented on combined PR #5; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-MAP-003 — Inline map execution did not enforce the timeout

- **Component:** `src/map.c`, worker-thread inline execution path
- **Severity/confidence:** P2 / high
- **Evidence:** The inline path previously executed every callback and returned success without checking the deadline.
- **Impact:** `taskforge_map_timeout` could exceed its requested timeout and continue starting later callbacks.
- **Remediation:** Check a monotonic deadline before and after each inline callback. Preserve a completed callback's result, stop starting later callbacks after the deadline, and report unstarted items as canceled. Running callbacks remain non-interruptible.
- **Regression test:** A worker invokes `taskforge_map_timeout_report` with a 50 ms deadline and a 150 ms first callback; the overall result must be `TIMEOUT`, the completed first item remains `OK`, and the second item is `CANCELLED`.
- **Status:** Merged via PR #7; all five gates passed in [run 37966033793](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966033793), and post-merge all-five-gate run 37966256429 passed.

### TF-QUEUE-002 — `queue_pop` returned false after an internal polling timeout

- **Component:** `src/queue.c`, `include/taskforge/queue.h`, `src/pool.c`
- **Severity/confidence:** P2 / medium-high
- **Evidence:** The public comment described a blocking pop, but the implementation used a 50 ms timed wait and returned false on an idle interval.
- **Impact:** Direct callers could interpret a temporary idle interval as a stopped queue.
- **Remediation:** Split the API: `queue_pop` now blocks until work or shutdown, while `queue_pop_timeout` is used by work-stealing workers for periodic local-deque polling.
- **Regression tests:** Empty-queue timed pop returns on timeout without stopping the queue; blocking pop remains blocked beyond 50 ms, wakes on a pushed task, and returns false when an empty queue is shut down.
- **Status:** Implemented on PR #8; CI run [37966656813](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966656813) passed all five gates on head `c2aff127140d1ff85f26973457d1d9a04ef71409`; the subsequent post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-DOC-001 — Repository has no license declaration

- **Component:** Repository root / distribution metadata
- **Severity/confidence:** P2 / high
- **Evidence:** The complete tracked-file tree contains no `LICENSE` file or license declaration.
- **Impact:** Public visibility does not itself grant downstream users permission to reuse, modify, or redistribute the project.
- **Proposed remediation:** Ask the repository owner to select a license and add the corresponding canonical license text; do not infer or add a license without owner authorization.
- **Required validation:** Confirm the selected license matches the owner's intent and is referenced from README.
- **Status:** Open; owner decision required.

### TF-BENCH-001 — Benchmark reports CSV success even when file creation fails

- **Component:** `benchmarks/bench_scaling.c`
- **Severity/confidence:** P3 / high
- **Evidence:** The benchmark continues if CSV `fopen` fails but still prints a success message claiming the CSV was saved.
- **Impact:** Automation and users may believe a benchmark artifact exists when it does not.
- **Proposed remediation:** Return a nonzero status or clearly mark artifact output as skipped/failed when the CSV cannot be opened.
- **Required regression test:** Run with an unwritable output directory and verify the process does not claim successful CSV output.
- **Status:** Fixed and merged via PR #9 at head `1483a9bc3831d90e06ce768e060f48a6fbb881cd`; PR CI run [37967278851](https://github.com/rushmanthnalluri/task-forge/actions/runs/37967278851) passed all five gates. Post-merge main CI [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.

### TF-FUTURE-001 — Cancellation after transition to RUNNING

- **Component:** `src/future.c`, `tests/test_futures.c`, `tests/test_future_cancel_transition.c`
- **Evidence:** Existing tests cover cancellation while queued, cleanup of canceled queued arguments, and immediate-shutdown cleanup. The transition boundary after a worker marks a future RUNNING lacked a deterministic regression.
- **Risk:** A future change could allow cancellation of executing work or incorrectly invoke the queued-task cleanup callback after execution starts.
- **Regression added:** The new test blocks inside a callback after it is RUNNING, asserts cancellation returns false, then releases it and checks completion, one execution, zero cleanup calls, and exactly-once argument disposal.
- **Status:** Candidate branch `test/future-cancel-transition-2026-10-10`, test commit `54e9429a0177bacb6632fb73ceeca98e2dc63a39`; exact-head CI pending.
## Subsystem coverage ledger

| Area | Reviewed | Remaining work |
|---|---|---|
| Repository tree / tracked paths | Complete recursive tree; 44 files | Inspect every remaining file and git history before claiming full semantic coverage |
| Pool lifecycle / resize / stats | Resize/statistics lifetime, idle global-queue retirement, concurrent submit/shutdown, and resize/shutdown regression tests reviewed | PR #13 and PR #14 exact-head CI plus post-merge main CI passed; cancellation-transition coverage remains open |
| Queue / producer backpressure | Source + bounded-queue test; concurrent timeout recovery and blocking/timed pop regressions added | All five gates passed on latest PR #8 head `c2aff127`; this documentation refresh needs revalidation |
| Futures / cancellation | Source + public API + tests | Audit ownership and cancellation under load |
| Map API | Source + tests; deterministic timeout, submission-failure, and inline-deadline regressions added | All five gates passed on latest PR #8 head `c2aff127`; inline timeout fix is merged |
| Parser / workload execution | Source + parser tests | More fuzz/property tests and resource-limit behavior |
| IPC | Source + header + tests; multiline, invalid argument, path safety, version, timeout, and error-fallback regressions added | Verify combined mission-branch CI; server_stop remains path-unlink only by documented contract |
| CLI | Entry point and command handling reviewed partially | Finish remaining command/signal/error-path review |
| Logging | Source reviewed | I/O error propagation and performance implications |
| Work stealing | Source reviewed | Model-based resize/shutdown interleavings |
| Build / CI | Makefile and workflow reviewed | Add dedicated static-analysis/security checks where justified |
| Docs / benchmarks / scripts | README, design, validation scripts and benchmark code inspected | CSV failure path now reports nonzero with regression coverage; missing LICENSE still requires owner decision |
| Dependency/security inventory | No manifest/lockfile found | Run compiler/static analyzer and secret scan in an executable environment |

## Baseline verification evidence

GitHub Actions run [37963052958](https://github.com/rushmanthnalluri/task-forge/actions/runs/37963052958) completed successfully on baseline commit `3c4b115da5e5bb14516d707858581aa7e62a240c` at 2026-10-09 16:59:25 UTC. This is prior-main evidence, not evidence for the mission branch.


## Mission update — 2026-10-10

- **Current main:** `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28` (merge commit for PR #10).
- **PR #10:** [Global-queue worker retirement fix](https://github.com/rushmanthnalluri/task-forge/pull/10) merged at `2026-10-10T01:31:03Z`; candidate head `ca06914d5fd0dbd65d9fcd659805c851c431a0da` passed all five required gates plus GitGuardian in [run 38013414941](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013414941).
- **Post-merge main CI:** [run 38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) completed successfully at `2026-10-10T01:31:55Z`; build/tests/CLI, ASan/UBSan, TSan, Valgrind Memcheck, and one-million-task soak all passed on `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28`.
- **TF-RESIZE-003 — Idle global-queue worker does not observe retirement:** fixed in PR #10. Global-queue workers now use internal timed pop polling so idle workers notice retirement; public `queue_pop` remains blocking. Regression in `tests/test_resize.c` shrinks an idle two-worker non-work-stealing pool to one and regrows it, with a 15-second alarm. Exact-head CI and post-merge CI passed.
- **Residual risks:** lifecycle interleavings involving concurrent submission and shutdown need more coverage; the fixed spare-worker capacity remains bounded; no license is declared (owner decision required); static analysis, dependency/security-alert scans beyond GitGuardian, and non-Ubuntu/platform compatibility remain unverified. No local commands were run because no local checkout is available.


## Mission update — lifecycle race coverage

- **Current main:** `08e67833e01695ea92dedd85a8a0644c74bfe320`; post-merge run [38013881871](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013881871) completed successfully at `2026-10-10T01:37:57Z` with all five required gates passing.
- **PR #13:** [Concurrent submission/shutdown regression](https://github.com/rushmanthnalluri/task-forge/pull/13), head `ad5dc7a6e9af5d538dba893f1bef86ca46d0d147`, merged at `2026-10-10T01:37:16Z` as `08e67833e01695ea92dedd85a8a0644c74bfe320`. Exact-head run [38013820528](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013820528) passed build/tests/CLI, ASan/UBSan, TSan, Valgrind Memcheck, one-million-task soak, and GitGuardian.
- **TF-LIFECYCLE-001 — submit/shutdown race coverage:** added in `tests/test_shutdown.c`. Concurrent producer and shutdown threads exercise graceful global-queue shutdown and immediate work-stealing shutdown; each accepted future must reach an allowed terminal state, and each submission argument must be executed or cleaned exactly once. A 30-second alarm bounds hangs. The test emitted explicit PASS lines in the observed build/test log; Valgrind reported zero errors for `test_shutdown`.
- **Residual risks:** cancellation state-transition races remain open; the `tests/test_map.c` compile log still reports implicit declarations for `pthread_create`/`pthread_join` (missing `<pthread.h>` include); no license is declared; static analysis, dependency/security-alert scans beyond GitGuardian, and non-Ubuntu compatibility remain unverified. No local commands were run.


## Mission update — resize/shutdown interleaving

- **Current main:** `a4d0d38bc12176df706af0fb7d9c2d8dfaa536a6`; post-merge [run 38014410578](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014410578) completed successfully at `2026-10-10T01:46:17Z`, all five required gates passed.
- **PR #14:** [Concurrent resize/shutdown regression](https://github.com/rushmanthnalluri/task-forge/pull/14), exact head `847e517c0ba8db8baa652b08f8cf73ed9eb98732`, merged at `2026-10-10T01:43:06Z` as `d34b9de4546772e99928798def5f5c2d502fa291`. Exact-head run [38014144625](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014144625) passed all five required gates plus GitGuardian.
- **TF-LIFECYCLE-002 — Resize/shutdown race coverage:** `tests/test_lifecycle_races.c` keeps a resizer cycling target worker counts while graceful shutdown starts. It accepts only `TASKFORGE_OK` or `TASKFORGE_ERR_SHUTDOWN`, requires multiple resize iterations, checks no unexpected status, uses a 15-second alarm, and joins the resizer before destroying the pool. CI logs show `[PASS] concurrent resize/shutdown serializes without invalid statuses` and `ALL DISCOVERED TESTS PASSED`.
- **Residual risks:** cancellation transition races remain open; `tests/test_map.c` has pre-existing implicit declarations for `pthread_create` and `pthread_join` because it lacks `<pthread.h>`; no license is declared; static analysis, security/dependency alert scans beyond GitGuardian, and non-Ubuntu compatibility remain unverified. No local commands were run.


## Mission update — 2026-10-10T01:46:17Z

- PR #16 reconciled the resize/shutdown records and merged as `a4d0d38bc12176df706af0fb7d9c2d8dfaa536a6`; exact-head run [38014353225](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014353225) and post-merge main run [38014410578](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014410578) passed all five gates.
- Latest verified main is `a4d0d38bc12176df706af0fb7d9c2d8dfaa536a6`; latest post-merge main CI completed at `2026-10-10T01:46:17Z` with build/tests/CLI, ASan/UBSan, TSan, Valgrind, and one-million-task soak all passing.
- New candidate `tests/test_future_cancel_transition.c` covers cancellation rejection after RUNNING; exact-head CI is pending.
