# TaskForge audit report

**Mission deadline:** 2026-10-10 12:50 IST (UTC+05:30)  
**Last confirmed execution time:** 2026-10-09 22:37 IST  
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

### TF-IPC-001 — IPC framing and request validation are incomplete
- **Component:** `src/ipc.c`, `include/taskforge/ipc.h`, `tests/test_ipc.c`
- **Severity/confidence:** P1 / high
- **Evidence:** Server parsing discards the parsed protocol version instead of validating it. Requests and responses use newline-delimited reads while also exposing byte lengths, so embedded newlines are truncated/misinterpreted. The client formats a request name without validating its length or whitespace, and response bodies are read as lines rather than by the declared byte length.
- **Impact:** Incorrect protocol behavior for valid byte payloads, malformed-request ambiguity, and interoperability hazards. The exposed local socket can also be affected by path lifecycle assumptions.
- **Proposed remediation:** Define and test explicit length framing, validate version/name/handler table/path before use, implement exact-length reads/writes with a total deadline, and preserve the host process's signal disposition.
- **Required tests:** Unknown protocol version; empty and maximum-length names/payloads; embedded newline bytes; partial reads/writes; malformed/oversized headers; timeout with a slow trickle; invalid handler tables.
- **Status:** Open; not yet changed.

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
- **Proposed remediation:** Populate per-item report statuses for futures canceled after the shared deadline and document that running tasks cannot be forcibly interrupted.
- **Status:** Open; not yet changed.

## Subsystem coverage ledger

| Area | Reviewed | Remaining work |
|---|---|---|
| Repository tree / tracked paths | Complete recursive tree; 44 files | Inspect every remaining file and git history before claiming full semantic coverage |
| Pool lifecycle / resize / stats | Targeted source review | Verify mission branch via CI; expand concurrent shutdown/resize/stats tests |
| Queue / producer backpressure | Source + bounded-queue test; concurrent timeout recovery added | Verify latest branch CI; strict FIFO producer fairness intentionally not guaranteed |
| Futures / cancellation | Source + public API + tests | Audit ownership and cancellation under load |
| Map API | Source + tests | Per-item report status on timeout |
| Parser / workload execution | Source + parser tests | More fuzz/property tests and resource-limit behavior |
| IPC | Source + header + tests | IPC hardening is being verified separately in PR #6 |
| CLI | Entry point and command handling reviewed partially | Finish remaining command/signal/error-path review |
| Logging | Source reviewed | I/O error propagation and performance implications |
| Work stealing | Source reviewed | Model-based resize/shutdown interleavings |
| Build / CI | Makefile and workflow reviewed | Add dedicated static-analysis/security checks where justified |
| Docs / benchmarks / scripts | README, design, and validation scripts inspected | Verify all claims/commands and benchmark reproducibility |
| Dependency/security inventory | No manifest/lockfile found | Run compiler/static analyzer and secret scan in an executable environment |

## Baseline verification evidence

GitHub Actions run [37963052958](https://github.com/rushmanthnalluri/task-forge/actions/runs/37963052958) completed successfully on baseline commit `3c4b115da5e5bb14516d707858581aa7e62a240c` at 2026-10-09 16:59:25 UTC. This is prior-main evidence, not evidence for the mission branch.
