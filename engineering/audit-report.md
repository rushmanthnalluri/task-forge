# TaskForge audit report

**Mission deadline:** 2026-10-10 12:50 IST (UTC+05:30)  
**Last confirmed execution time:** 2026-10-09 22:42 IST  
**Baseline branch:** `main`  
**Baseline commit:** `3c4b115da5e5bb14516d707858581aa7e62a240c`  
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
- **Status:** Implemented on mission branch; CI pending.
- **Commit:** `ce43a233a47d3ae9acf71b531b5257129764cb09` (source fix).

### TF-RESIZE-002 — Statistics race with worker resize/deque destruction
- **Component:** `src/pool.c`, `taskforge_pool_get_stats`
- **Severity/confidence:** P1 / high
- **Evidence:** Stats read mutable `config.num_workers` while resize wrote it, and traversed local deques that resize destroyed after joining workers. A concurrent stats call could race with the configuration write or touch a destroyed deque mutex.
- **Impact:** Data race, undefined behavior, or a hang/crash during concurrent stats and resize.
- **Remediation:** Derive worker count from the atomic operational-worker count. Retain initialized deques through shrink/re-growth and destroy them only after shutdown at pool destruction.
- **Regression test:** Same resize/stats regression in `tests/test_resize_stress.c`.
- **Status:** Implemented on mission branch; CI pending.
- **Commit:** `ce43a233a47d3ae9acf71b531b5257129764cb09`.

### TF-IPC-001 — IPC response framing, protocol checks, and read deadlines

- **Severity/confidence:** P2 / high.
- **Evidence:** The baseline client read response bodies with a newline reader even though the response header carried a byte length; the server parsed but did not enforce the protocol version.
- **Impact:** Multiline handler results were truncated/misframed and incompatible protocol versions could reach dispatch.
- **Remediation:** Read the advertised body length exactly, validate the trailing delimiter and response header, enforce protocol version, validate handler names/entries, and use a shared monotonic deadline across the response header/body.
- **Regression tests:** Multiline result round-trip; unsupported version against a registered handler; slow-trickle body must time out against one total deadline.
- **Status:** Implemented on the mission branch; combined CI pending.

### TF-IPC-002 — Unsafe socket-path deletion

- **Severity/confidence:** P1 / high.
- **Evidence:** Baseline startup unconditionally unlinked the requested path before bind and also unlinked after bind/listen failure. The stop function unlinked any path.
- **Impact:** A caller-supplied regular file could be deleted, or a path not created by this server could be removed.
- **Remediation:** Reject existing paths, do not unlink on bind failure, record the bound socket's device/inode and only clean up that same socket, and reject non-socket paths in the cleanup API.
- **Regression test:** A pre-existing regular file remains byte-for-byte unchanged after attempted server start/stop.
- **Status:** Implemented on the mission branch; combined CI pending.

### TF-IPC-003 — Ambiguous request text was not rejected

- **Severity/confidence:** P2 / high.
- **Evidence:** The v1 request format uses a newline-terminated text line; NUL, CR, or LF inside the argument cannot be represented unambiguously.
- **Impact:** Such input could be truncated or interpreted as protocol framing.
- **Remediation:** Explicitly reject NUL/CR/LF request arguments before connecting; keep the supported v1 argument contract text-only. Response bodies are length-framed and can contain newlines.
- **Regression test:** Client calls with NUL/CR/LF arguments fail before connecting.
- **Status:** Implemented on the mission branch; combined CI pending.

### TF-IPC-004 — Handler failure could be reported as success

- **Severity/confidence:** P2 / high.
- **Evidence:** A handler returning failure while leaving its error output zero could produce a failure response with error code zero, which the client returned as success.
- **Impact:** Callers could mistake failed work for successful work.
- **Remediation:** Normalize failure-with-zero-error to `TASKFORGE_ERR_FAILED` on both server and client paths.
- **Regression test:** A handler that fails without setting an error code must return `TASKFORGE_ERR_FAILED`.
- **Status:** Implemented on the mission branch; combined CI pending.

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
- **Status:** Implemented on `mission/engineering-hardening-2026-10-09`; combined CI pending.

### TF-MAP-001 — Timed map per-item reports do not describe all canceled items
- **Component:** `src/map.c`, `taskforge_map_timeout_report`
- **Severity/confidence:** P2 / medium
- **Evidence:** On timeout at index `i`, later futures are canceled and released, but their report entries retain the initial generic failure values rather than the resulting canceled state/status.
- **Impact:** Callers cannot reliably distinguish timed-out, canceled, and failed items in the report.
- **Remediation:** When the shared deadline expires, cancel the current item if it is still pending; for later items, record `CANCELLED` when cancellation succeeds or perform a zero-time terminal-state check so completed, failed, and still-running items are reported accurately. Running tasks are not forcibly interrupted.
- **Regression test:** A one-worker pool runs a slow first item and leaves the second queued; the report must mark the first `TIMEOUT` and the second `CANCELLED`.
- **Status:** Implemented on `mission/engineering-hardening-2026-10-09`; combined CI pending.

## Subsystem coverage ledger

| Area | Reviewed | Remaining work |
|---|---|---|
| Repository tree / tracked paths | Complete recursive tree; 44 files | Inspect every remaining file and git history before claiming full semantic coverage |
| Pool lifecycle / resize / stats | Targeted source review | Verify mission branch via CI; expand concurrent shutdown/resize/stats tests |
| Queue / producer backpressure | Source + bounded-queue test; concurrent timeout recovery added | Verify latest branch CI; strict FIFO producer fairness intentionally not guaranteed |
| Futures / cancellation | Source + public API + tests | Audit ownership and cancellation under load |
| Map API | Source + tests; deterministic one-worker timeout report regression added | Verify latest mission-branch CI |
| Parser / workload execution | Source + parser tests | More fuzz/property tests and resource-limit behavior |
| IPC | Source + header + tests; multiline, invalid argument, path safety, version, timeout, and error-fallback regressions added | Verify combined mission-branch CI; server_stop remains path-unlink only by documented contract |
| CLI | Entry point and command handling reviewed partially | Finish remaining command/signal/error-path review |
| Logging | Source reviewed | I/O error propagation and performance implications |
| Work stealing | Source reviewed | Model-based resize/shutdown interleavings |
| Build / CI | Makefile and workflow reviewed | Add dedicated static-analysis/security checks where justified |
| Docs / benchmarks / scripts | README, design, and validation scripts inspected | Verify all claims/commands and benchmark reproducibility |
| Dependency/security inventory | No manifest/lockfile found | Run compiler/static analyzer and secret scan in an executable environment |

## Baseline verification evidence

GitHub Actions run [37963052958](https://github.com/rushmanthnalluri/task-forge/actions/runs/37963052958) completed successfully on baseline commit `3c4b115da5e5bb14516d707858581aa7e62a240c` at 2026-10-09 16:59:25 UTC. This is prior-main evidence, not evidence for the mission branch.
