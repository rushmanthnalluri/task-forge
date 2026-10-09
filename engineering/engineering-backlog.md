# Engineering backlog

**Updated:** 2026-10-09 23:02 IST  
**Deadline:** 2026-10-10 12:50 IST  
**Latest green main head:** `02feaf1dd269f77c1f0265155863f8f05ddeeb7f`; post-merge run [37966256429](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966256429) passed all five gates.  
**Latest green queue-pop PR head:** `3e3ebe6918d92ced72b0ea7914523bfd508b651c`; run [37966854175](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966854175) passed all five gates. This documentation refresh requires revalidation.

Status values: **Done** means implemented and verified at the cited revision; **Open** means not implemented; **Blocked** means unable to verify with available tools.

## P1 — correctness and reliability

- [x] **RESIZE-1: Remove callback deadlock during shrink.** Worker callbacks can query worker count/stats while a concurrent resize joins retiring workers. Regression: `tests/test_resize_stress.c`. Verified by CI.
- [x] **RESIZE-2: Make stats safe during resize.** Use atomic operational worker count and retain initialized deques until pool destruction. TSan passed.
- [x] **IPC-1: Harden protocol framing and validation.** Enforce protocol version, validate handler tables/headers, read length-framed responses exactly, and apply a shared monotonic response deadline. Text-only request arguments reject NUL/CR/LF.
- [x] **IPC-2: Protect socket paths.** Reject pre-existing paths; cleanup only the socket inode created by this server; do not remove arbitrary files on bind failure.
- [x] **QUEUE-1: Remove allocation-dependent producer cancellation bookkeeping.** Eliminates the realloc-failure path that could strand producers. Trade-off: strict FIFO producer fairness is not promised and is documented.
- [ ] **LIFECYCLE-1: Test submit/shutdown and resize/shutdown races.** Acceptance: concurrent calls terminate deterministically; accepted-task outcomes are documented; no worker is joined twice. Clarify that all concurrent API callers must finish before destroy.

## P2 — correctness, observability, and test depth

- [x] **MAP-1: Correct timeout report statuses.** Timed-out, canceled, completed, and still-running items are distinguished; running tasks are not claimed to have been interrupted.
- [x] **MAP-2: Preserve per-item reports when a later submission fails.** Drain prior futures and populate their actual status, error, and result. Regression uses a one-worker/capacity-one pool with concurrent immediate shutdown.
- [x] **MAP-3: Enforce timeout semantics for inline/nested map calls.** Worker-inline execution checks the monotonic deadline between callbacks, preserves completed results, and reports unstarted items as canceled. Regression verified by all five gates in run [37966033793](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966033793).
- [x] **QUEUE-2: Make `queue_pop` semantics match its contract.** `queue_pop` blocks until work or shutdown; work-stealing workers use `queue_pop_timeout` for polling. Regression tests cover idle timeout, blocking wake-on-push, and shutdown wakeup. All five gates passed on PR #8 head `3e3ebe69` in [run 37966854175](https://github.com/rushmanthnalluri/task-forge/actions/runs/37966854175).
- [ ] **FUTURE-1: Expand cancellation race tests.** Cover cancellation before dequeue, the transition to RUNNING, and immediate shutdown with exactly-once cleanup.
- [ ] **PARSER-1: Add parser property/fuzz coverage.** Malformed, huge, and boundary-valued specs must fail safely without integer overflow or unbounded unexpected allocation.
- [ ] **LOG-1: Review logger I/O failure behavior.** Define what happens when write/flush fails and ensure logging failures cannot corrupt pool lifecycle.
- [ ] **CLI-1: Finish review of all commands and signal/shutdown paths.** Verify documented commands, invalid/oversized input handling, and interrupt behavior.

## P3 — maintainability, security, and developer experience

- [ ] **DOCS-LICENSE-1: Resolve missing license declaration.** The repository has no `LICENSE` file. This requires the owner's license choice; do not add a license by inference. Once selected, add the canonical text and reference it from README.
- [ ] **BENCH-1: Fix false CSV-success reporting.** `benchmarks/bench_scaling.c` prints that CSV was saved even if opening the output file fails. Return failure or clearly report the artifact as unavailable; test an unwritable output path.
- [ ] **CI-1: Add a supported static-analysis pass.** Evaluate compiler diagnostics and clang-tidy/cppcheck availability before adding a tool.
- [ ] **SEC-1: Run secret/dependency scanning appropriate to the repository.** No package manifest or lockfile is present; the connected GitHub API did not expose secret/dependency/code-scanning alert endpoints, so these scans remain unverified.
- [ ] **DOCS-1: Reconcile README/design claims with verified behavior and supported platforms.** Keep benchmark claims reproducible and distinguish Linux-tested behavior from untested POSIX platforms.

## Release gate

Before merge, the latest combined head must pass targeted tests, full `make test`, build/CLI, ASan/UBSan, TSan, Valgrind, and the one-million-task soak. A green result on an earlier SHA is not a pass for a later code revision. Documentation-only edits still need a final CI run before merge.
