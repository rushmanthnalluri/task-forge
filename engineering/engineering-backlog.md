# Engineering backlog

**Latest verified CI completion:** 2026-10-10T01:52:43Z (2026-10-10 07:22:43 IST)  
**Deadline:** 2026-10-10 12:50 IST  
**Latest green main head:** `022f3430da6ddb969b4d9c0fa0c92abae6749a6d`; post-merge run [38014822336](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014822336) passed all five gates.  
**Queue-pop fix:** PR #8 merged; all five gates passed on its exact head and subsequent main CI passed. **Benchmark CSV fix:** PR #9 merged; exact-head and post-merge CI passed.

Status values: **Done** means implemented and verified at the cited revision; **Open** means not implemented; **Blocked** means unable to verify with available tools.

## P1 — correctness and reliability

- [x] **RESIZE-1: Remove callback deadlock during shrink.** Worker callbacks can query worker count/stats while a concurrent resize joins retiring workers. Regression: `tests/test_resize_stress.c`. Verified by CI.
- [x] **RESIZE-2: Make stats safe during resize.** Use atomic operational worker count and retain initialized deques until pool destruction. TSan passed.
- [x] **RESIZE-3: Let idle global-queue workers retire during shrink.** Use timed internal queue polling while preserving public `queue_pop`; regression covers idle shrink/regrow. PR #10 head passed all five gates plus GitGuardian, and post-merge main run [38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895) passed all five gates.
- [x] **IPC-1: Harden protocol framing and validation.** Enforce protocol version, validate handler tables/headers, read length-framed responses exactly, and apply a shared monotonic response deadline. Text-only request arguments reject NUL/CR/LF.
- [x] **IPC-2: Protect socket paths.** Reject pre-existing paths; cleanup only the socket inode created by this server; do not remove arbitrary files on bind failure.
- [x] **QUEUE-1: Remove allocation-dependent producer cancellation bookkeeping.** Eliminates the realloc-failure path that could strand producers. Trade-off: strict FIFO producer fairness is not promised and is documented.
- [x] **LIFECYCLE-1: Cover submit/shutdown and resize/shutdown races.** PR #13 tests graceful global-queue and immediate work-stealing shutdown against concurrent submission, terminal future outcomes, and exactly-once argument disposal. PR #14 adds a resizer racing graceful shutdown and accepts only documented statuses. PR #13 and PR #14 exact-head CI and their post-merge main runs passed all five gates.

## P2 — correctness, observability, and test depth

- [x] **MAP-1: Correct timeout report statuses.** Timed-out, canceled, completed, and still-running items are distinguished; running tasks are not claimed to have been interrupted.
- [x] **MAP-2: Preserve per-item reports when a later submission fails.** Drain prior futures and populate their actual status, error, and result. Regression uses a one-worker/capacity-one pool with concurrent immediate shutdown.
- [x] **MAP-3: Enforce timeout semantics for inline/nested map calls.** Worker-inline execution checks the monotonic deadline between callbacks, preserves completed results, and reports unstarted items as canceled. Regression verified by all five gates in run [37966033793](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966033793).
- [x] **QUEUE-2: Make `queue_pop` semantics match its contract.** `queue_pop` blocks until work or shutdown; work-stealing workers use `queue_pop_timeout` for polling. Regression tests cover idle timeout, blocking wake-on-push, and shutdown wakeup. All five gates passed on PR #8 head `3e3ebe69` in [run 37966854175](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966854175).
- [x] **FUTURE-1: Expand cancellation race tests.** PR #17 covers pending cancellation vs RUNNING, 200 scheduling trials, immediate shutdown, terminal states, and exactly-once argument disposal. Latest-head and post-merge main CI passed all five required gates.
- [ ] **PARSER-1: Add parser property/fuzz coverage.** Malformed, huge, and boundary-valued specs must fail safely without integer overflow or unbounded unexpected allocation.
- [ ] **LOG-1: Review logger I/O failure behavior.** Define what happens when write/flush fails and ensure logging failures cannot corrupt pool lifecycle.
- [ ] **CLI-1: Finish review of all commands and signal/shutdown paths.** Verify documented commands, invalid/oversized input handling, and interrupt behavior.

## P3 — maintainability, security, and developer experience

- [ ] **DOCS-LICENSE-1: Resolve missing license declaration.** The repository has no `LICENSE` file. This requires the owner's license choice; do not add a license by inference. Once selected, add the canonical text and reference it from README.
- [x] **BENCH-1: Fix false CSV-success reporting.** The benchmark fails clearly if CSV creation/writes/close fail; `tests/test_bench_output.sh` verifies nonzero status and actionable error. Merged via PR #9; exact-head CI and post-merge main CI passed.
- [ ] **CI-1: Add a supported static-analysis pass.** Evaluate compiler diagnostics and clang-tidy/cppcheck availability before adding a tool.
- [ ] **SEC-1: Run secret/dependency scanning appropriate to the repository.** No package manifest or lockfile is present; the connected GitHub API did not expose secret/dependency/code-scanning alert endpoints, so these scans remain unverified.
- [ ] **DOCS-1: Reconcile README/design claims with verified behavior and supported platforms.** Keep benchmark claims reproducible and distinguish Linux-tested behavior from untested POSIX platforms.

## Latest mission state (2026-10-10)

- Main `98ac2418ffcd9a933da98c1aef8b8ff494c7fa28` is green in [run 38013479895](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013479895).
- PR #10 merged at `2026-10-10T01:31:03Z`; its exact head passed build/tests/CLI, ASan/UBSan, TSan, Valgrind, one-million-task soak, and GitGuardian in [run 38013414941](https://github.com/rushmanthnalluri/task-forge/actions/runs/38013414941).
- Next priority: **IPC-STOP-1**: make `taskforge_ipc_server_stop` wake a server blocked in `accept()` without unlinking a replacement socket or caller-owned path. Define and test the stop protocol before implementation. Do not claim unrun local tests.
- Remaining: cancellation-transition race coverage; fix implicit pthread declarations in `tests/test_map.c`; license owner decision; static analysis; dependency/security alert scans beyond GitGuardian; non-Ubuntu compatibility; bounded worker-capacity trade-off.

## Release gate

Before merge, the latest combined head must pass targeted tests, full `make test`, build/CLI, ASan/UBSan, TSan, Valgrind, and the one-million-task soak. A green result on an earlier SHA is not a pass for a later code revision. Documentation-only edits still need a final CI run before merge.


**Evidence timestamp:** 2026-10-10T01:37:57Z (2026-10-10 07:07:57 IST). The documentation revision itself is not yet CI-verified; do not merge its documentation PR until the exact latest head passes all required gates.


**Evidence timestamp:** 2026-10-10T01:37:57Z (2026-10-10 07:07:57 IST). Documentation-only changes require their own exact-head CI before merge.


**Evidence timestamp:** 2026-10-10T01:52:43Z (2026-10-10 07:22:43 IST). Current-main verification is from GitHub Actions only; no local checkout was available.


- [x] **TEST-PORT-1: Declare pthread APIs in map tests.** PR #18 adds `<pthread.h>`; exact-head and post-merge build logs no longer show implicit-declaration warnings. Run [38014763502](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014763502) and main [38014822336](https://github.com/rushmanthnalluri/task-forge/actions/runs/38014822336) passed required gates.

**Evidence timestamp:** 2026-10-10T01:52:43Z (2026-10-10 07:22:43 IST).
