# Engineering backlog

**Updated:** 2026-10-09 22:32:41 IST  
**Deadline:** 2026-10-10 12:50 IST

Status values: **Done on branch** means implemented but not yet CI-verified; **Open** means not implemented; **Blocked** means unable to verify with currently available tools.

## P1 — correctness and reliability

- [x] **RESIZE-1: Remove callback deadlock during shrink.** Acceptance: worker callbacks can call `taskforge_pool_worker_count()` while another thread shrinks the pool; regression test completes within the test timeout. Status: Done on branch; CI pending.
- [x] **RESIZE-2: Make stats safe during resize.** Acceptance: stats uses an atomic worker-count snapshot and never locks a deque that resize has destroyed; repeated shrink/grow and callback stats queries pass under TSan. Status: Done on branch; CI pending.
- [ ] **IPC-1: Correct request/response framing.** Acceptance: exact-length body handling, version checks, embedded newline payloads, malformed input rejection, and bounded total timeout are covered by tests.
- [ ] **QUEUE-1: Make producer cancellation bookkeeping failure-safe.** Acceptance: simulated allocation failure cannot strand the producer turn; blocked producers either progress or receive a documented error.
- [ ] **LIFECYCLE-1: Test submit/shutdown and resize/shutdown races.** Acceptance: concurrent calls terminate deterministically, accepted tasks have documented outcomes, and no worker is joined twice.

## P2 — correctness, observability, and test depth

- [ ] **MAP-1: Correct per-item timeout report statuses.** Acceptance: timed-out and canceled items have distinct, documented report status; running tasks are not claimed to have been interrupted.
- [ ] **FUTURE-1: Expand cancellation race tests.** Acceptance: cancellation before dequeue, racing with transition to RUNNING, and immediate shutdown each have exactly-once cleanup and stable terminal state.
- [ ] **PARSER-1: Add parser property/fuzz coverage.** Acceptance: malformed, huge, and boundary-valued specifications fail safely without integer overflow or unbounded unexpected allocation.
- [ ] **IPC-2: Validate server handler table and socket path.** Acceptance: null names/callbacks, overlong paths, and unsafe existing path types are rejected without deleting unrelated filesystem entries.
- [ ] **LOG-1: Review logger failure behavior.** Acceptance: file write/flush failures have a documented policy and cannot corrupt pool lifecycle.
- [ ] **CLI-1: Complete review of every command, signal handling, and shutdown path.** Acceptance: documented commands match actual parser behavior and invalid/oversized input is handled predictably.

## P3 — maintainability and developer experience

- [ ] **CI-1: Add a supported static-analysis pass.** Evaluate compiler warnings plus clang-tidy/cppcheck availability; do not add tools without reliable CI support.
- [ ] **SEC-1: Add secret/dependency scanning appropriate to a C-only repository.** No package manifest or lockfile exists in the current tree; identify actual applicable tooling first.
- [ ] **DOCS-1: Reconcile README/design claims with current behavior and CI.** Remove any unverified absolute claims and list reproducible commands.
- [ ] **BENCH-1: Verify benchmark methodology and artifact generation.** Report only measured, repeatable results; avoid treating host-specific results as universal.

## Release gate

Before merge: targeted regression tests, full `make test`, CI build/CLI, ASan/UBSan, TSan, Valgrind, and one-million-task soak must pass when the workflow supports those gates. Any unavailable check must be marked blocked rather than inferred as passed. The currently observed CI workflow is the source of truth for actually automated gates.
