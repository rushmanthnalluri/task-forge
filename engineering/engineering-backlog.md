# TaskForge Engineering Backlog

**Updated:** 2026-10-09 22:31 IST  
**Branch under active IPC work:** `fix/ipc-protocol-framing-main`

## P0 — Critical

- None confirmed in the reviewed scope.

## P1 — High

- [x] **IPC-01: Prevent deletion of unrelated filesystem paths.** Reject existing socket paths on startup, avoid unlinking on bind failure, and only clean up the socket inode created by this server. Regression test preserves regular-file bytes. **Done when:** path-safety test and full CI pass on the combined branch.
- [x] **CONC-01: Address worker-pool initialization/lifecycle race observed by TSan.** Subsequent changes are present on current `main`; CI run #176 reports all jobs green. **Done when:** TSan remains green on the final combined revision.
- [ ] **REVIEW-01: Complete independent concurrency review.** Review resize/shutdown lock ordering, worker publication, work-stealing drain predicates, and accepted-task completion. **Done when:** invariants and tests cover each state transition; any new race finding has a regression test.

## P2 — Medium

- [x] **IPC-02: Respect length-prefixed response framing.** Exact-length body reads and delimiter validation; multiline regression test added. **Done when:** IPC tests and CI pass.
- [x] **IPC-03: Enforce protocol version.** Known-handler request with unsupported version must be rejected. **Done when:** regression test and CI pass.
- [x] **IPC-04: Never return success for a failed handler with no error code.** Normalize to `TASKFORGE_ERR_FAILED`; add regression test. **Done when:** regression test and CI pass.
- [x] **IPC-05: Apply one monotonic deadline to the response header and body.** Added a slow-trickle body regression test. **Done when:** total-deadline test and all CI jobs pass.
- [ ] **CORE-01: Audit queue and future ownership/state transitions.** Focus on cancellation races, cleanup callback exactly-once semantics, timed waits, and reference-count lifecycle. **Done when:** each finding has evidence and meaningful regression coverage.
- [ ] **CORE-02: Audit parser and CLI malformed-input behavior.** Check integer overflow, malformed workload input, error propagation, and shutdown behavior. **Done when:** fuzz/edge tests or table-driven tests cover boundaries.
- [ ] **CORE-03: Audit map and work-stealing concurrency contracts.** Validate nested calls, deadline semantics, local-deque draining, and resize interactions. **Done when:** documented invariants match code and TSan/ASan tests pass.
- [x] **DOC-01: Clarify socket API path ownership and stale-socket behavior.** Existing paths are rejected and stale sockets require explicit owner cleanup; the header and README explain this. **Done when:** documentation matches behavior and CI passes.
- [x] **IPC-06: Clarify `taskforge_ipc_server_stop` semantics.** Document that it unlinks a socket pathname but does not terminate a listening server; a true stop protocol remains a future API design decision.
- [ ] **DOC-02: Document public API thread-safety/lifecycle contracts.** Align with tests and implementation; do not promise unsupported concurrent destruction. **Done when:** design docs and header comments agree.
- [ ] **LEGAL-01: Select and add a repository license.** **Blocked:** owner must choose a license. Do not guess or apply one automatically.

## P3 — Low

- [ ] **DOC-03: Update README file inventory.** Include IPC and worker-resize stress tests. **Done when:** listed paths match current repository tree.
- [ ] **DEVEX-01: Check build target reproducibility and diagnostics.** Review Makefile clean/rebuild behavior, compiler portability, and benchmark smoke assumptions. **Done when:** results and supported toolchain scope are documented.

## Verification gate

- Required before merging any concurrency or IPC change: Build/tests/CLI, ASan+UBSan, TSan, Valgrind, and million-task soak.
- Existing evidence: `main` CI run #176 is green on `3c4b115da5e5bb14516d707858581aa7e62a240c`.
- New IPC branch CI: not yet observed; do not mark it passed until GitHub reports final success.
