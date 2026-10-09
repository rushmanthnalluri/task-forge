# TaskForge Engineering Audit

**Last reviewed:** 2026-10-09 22:31 IST  
**Repository:** `rushmanthnalluri/task-forge`  
**Baseline reviewed:** `main` at `3c4b115da5e5bb14516d707858581aa7e62a240c`  
**Audit scope:** repository tree enumerated; detailed source review performed for IPC protocol/socket lifecycle and worker-pool initialization/shutdown paths. This is an in-progress audit, not a claim that every source file has been fully reviewed.

## Executive status

- The current `main` CI run #176 completed successfully across build/tests/CLI, ASan+UBSan, TSan, Valgrind, and the one-million-task soak test.
- IPC fixes are being prepared on `fix/ipc-protocol-framing-main`, based on the latest `main`.
- The earlier PR #3 CI run #172 failed in TSan on its older base; the report showed a read/write race in pool initialization. The newer `main` CI run #176 passed TSan after subsequent lifecycle fixes. The old failure is retained as evidence rather than erased.
- No local checkout is available in the current execution container; verification relies on GitHub Actions and repository file inspection. Do not claim local test execution.

## Findings

### TF-IPC-001 — IPC server can unlink a caller-owned path

- **Severity:** P1 (data-loss risk); **confidence:** high.
- **Evidence:** baseline `src/ipc.c` unconditionally called `unlink(path)` before `bind()`, and also unlinked the path after bind/listen failure. `taskforge_ipc_server_stop()` also unlinked any supplied path without checking its file type.
- **Impact:** passing an existing regular-file path could delete that file; a failed bind could also remove a path the server did not create.
- **Remediation:** reject any pre-existing path before binding; never unlink on bind failure; capture the bound socket's device/inode and only remove that same socket on cleanup; make `server_stop` reject non-socket paths.
- **Regression test:** pre-create a regular file, assert server start/stop fail, and verify file bytes remain unchanged.
- **Status:** implemented on `fix/ipc-protocol-framing-main`; remote CI pending at time of writing.

### TF-IPC-002 — Length-prefixed response body was read as a line

- **Severity:** P2; **confidence:** high.
- **Evidence:** baseline client used `read_line()` to consume a response body even though the header advertised a byte length.
- **Impact:** valid handler results containing newlines could be truncated or misframed.
- **Remediation:** read exactly the advertised byte count and validate the required trailing newline; reject invalid lengths and response status values.
- **Regression test:** a handler returns `first line\nsecond line`; client must receive the exact string.
- **Status:** implemented on `fix/ipc-protocol-framing-main`; remote CI pending.

### TF-IPC-003 — Unsupported request protocol versions were not rejected reliably

- **Severity:** P2; **confidence:** high.
- **Evidence:** baseline server parsed the version into a temporary value but did not compare it with `TASKFORGE_IPC_VERSION`.
- **Impact:** a request from an incompatible protocol version could be dispatched to a registered handler.
- **Remediation:** require the supported version before handler lookup.
- **Regression test:** send version 999 while naming the registered `echo` handler; server must reject it.
- **Status:** implemented on `fix/ipc-protocol-framing-main`; remote CI pending.

### TF-IPC-004 — Handler failure with no error code could be reported as success

- **Severity:** P2; **confidence:** high.
- **Evidence:** a handler returning failure while leaving its error output at zero produced a failure response with error code zero; client return logic could therefore return zero.
- **Impact:** callers may treat failed work as successful.
- **Remediation:** map failure-with-zero-error to `TASKFORGE_ERR_FAILED` on the server and retain the same fallback in the client.
- **Regression test:** a deliberately failing handler that does not set an error code must return `TASKFORGE_ERR_FAILED`.
- **Status:** implemented on `fix/ipc-protocol-framing-main`; remote CI pending.

### TF-CONC-001 — ThreadSanitizer reported a pool-initialization race on an older base

- **Severity:** P1 pending investigation; **confidence:** high that TSan reported a race on the tested revision.
- **Evidence:** PR #3 run #172 reported a write in `taskforge_pool_create` racing a read in `worker_loop` in `src/pool.c`; build/tests, ASan+UBSan, Valgrind, and the million-task soak jobs passed that run, but TSan failed.
- **Impact:** unsynchronized publication of worker state can cause undefined behavior.
- **Remediation/status:** subsequent pool lifecycle hardening is now on `main`; CI run #176 passed the TSan job. Preserve this finding as historical evidence and rerun TSan on any combined IPC branch before merge.

### TF-DOC-001 — Repository reuse rights are unclear

- **Severity:** P2; **confidence:** high.
- **Evidence:** repository metadata reports no license, and the enumerated tree contains no root license file.
- **Impact:** downstream users cannot infer permission to use, modify, or redistribute the project.
- **Remediation:** repository owner must choose the intended license; do not add a license without that decision.
- **Status:** blocked on owner choice.

### TF-DOC-002 — README inventory is behind the repository tree

- **Severity:** P3; **confidence:** high.
- **Evidence:** the tree contains IPC headers/source/tests and resizing stress tests not fully represented in the README's directory inventory.
- **Impact:** contributors can miss existing modules and test coverage.
- **Remediation:** update the directory inventory and document IPC path ownership, response framing, and supported platform scope after the IPC API contract is settled.
- **Status:** open.

## Coverage ledger

| Area | Coverage in this iteration | Status |
|---|---|---|
| Repository tree / tracked paths | Recursive Git tree enumerated at the current branch and main | Complete for enumeration |
| IPC implementation and public header | Full files inspected | Detailed review; fixes in progress |
| IPC regression tests | Full test file inspected and expanded | Detailed review; CI pending |
| Worker-pool lifecycle | `src/pool.c` inspected for TSan race and shutdown/drain paths | Partial; CI #176 green |
| Makefile and CI workflow | Full files inspected | Reviewed |
| README and design docs | README inspected; other docs inventoried | Partial |
| Queue / future / parser / map / logging / work-stealing internals | Paths inventoried; not all implementations reviewed line-by-line in this iteration | Open |
| CLI and benchmark implementation | Paths inventoried; not fully audited in this iteration | Open |
| Dependency / license / supply-chain review | No package manifest or third-party dependency lockfile in tree; license absence identified | Partial |
| Windows support | Source has a `_WIN32` stub; README defines Linux/POSIX scope | Intentional limitation unless support requirements change |

## Release gate

Do not claim a complete repository audit or merge the IPC branch until the combined branch passes current CI, including TSan. Keep license selection as an owner decision and continue the unaudited subsystem review.
