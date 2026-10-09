# Research log

**Updated:** 2026-10-09 22:49 IST

## R-001 — POSIX thread lifecycle and lock ordering
- **Question:** Can a pool resize hold a mutex while joining a worker whose callback may call back into the pool?
- **Primary references:**
  - POSIX `pthread_join`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_join.html
  - POSIX mutex locking: https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html
- **Repository-specific evidence:** `taskforge_pool_resize` held `resize_mutex` across `pthread_join`; `taskforge_pool_worker_count` acquired that same mutex. A worker callback calling the count API could wait for the resizer while the resizer waited for that callback to return.
- **Decision:** Make worker-count reads atomic; keep worker-local deque mutexes alive during shrink/re-growth to avoid stats racing with destruction. This is a narrow change without introducing another condition variable or changing the public API.
- **Risk / verification:** Worker storage is fixed-capacity. Pool destruction still must not race with any other pool operation, as documented. CI must validate the new regression under TSan and normal tests.
- **Outcome:** Implemented on mission branch in commit `ce43a233a47d3ae9acf71b531b5257129764cb09`; CI run [37965261107](https://github.com/rushmanthnalluri/task-forge/actions/runs/37965261107) passed all five gates on code/test head `b6c9eacaa42e724d6dec0258ac7e5ec725f1304a`; later documentation-only changes require revalidation.

## R-002 — IPC framing
- **Question:** Does the current line protocol safely represent the byte lengths exposed by the API?
- **Primary references:**
  - POSIX `recv`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/recv.html
  - POSIX `send`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/send.html
- **Repository-specific evidence:** Client API accepts pointer plus byte length, and response header carries a body length, but both request and response bodies are read using newline-delimited logic. The server parses but does not validate the request protocol version.
- **Options:** (A) Restrict v1 to newline-free text and reject ambiguous arguments; (B) change request wire framing and bump the protocol version.
- **Decision:** Keep v1 request arguments text-only and reject NUL/CR/LF before connecting, avoiding silent truncation without an unplanned wire-format break. Length-frame response bodies, where the protocol already advertises an explicit result length, and validate the trailing delimiter. Use one monotonic deadline for response header and body.
- **Regression coverage:** multiline result round-trip, unsupported protocol version, invalid request text, and a slow-trickle body deadline. Combined CI run [37965261107](https://github.com/rushmanthnalluri/task-forge/actions/runs/37965261107) passed all five gates on code/test head `b6c9eacaa42e724d6dec0258ac7e5ec725f1304a`; later documentation-only changes require revalidation.

## R-003 — Producer-ticket cancellation under allocation failure
- **Question:** Can queue fairness bookkeeping fail without losing liveness?
- **Repository-specific evidence:** `cancel_ticket` allocates a dynamically growing canceled-ticket array. `abandon_ticket` ignores allocation failure, potentially leaving a ticket gap that prevents `producer_turn` from advancing.
- **Options:** Remove ticket-based FIFO admission, use allocation-free bounded bookkeeping, or make the queue fail closed and deterministically resolve all waiting producers.
- **Decision:** Remove the ticket admission/cancellation ledger. The queue mutex already serializes insertion and the public contract does not promise FIFO ordering among producers. This avoids a heap allocation for every canceled future ticket and eliminates the failure mode where a failed `realloc` silently loses a ticket. The public header now states that producer wake-up order is scheduler-dependent.
- **Validation:** Add a bounded-queue regression with many concurrent timed-out producers and verify subsequent pushes/pops still work. Combined CI run [37965261107](https://github.com/rushmanthnalluri/task-forge/actions/runs/37965261107) passed all five gates on code/test head `b6c9eacaa42e724d6dec0258ac7e5ec725f1304a`.
- **Trade-off:** producer fairness is not guaranteed. If strict FIFO fairness becomes a requirement, redesign it with a bounded or allocation-safe waiter structure and test its failure paths before reintroducing ticketing.


## R-004 — Accurate per-item map timeout reporting

- **Question:** After a shared map deadline expires, how should later items appear in the per-item report?
- **Repository-specific evidence:** `map_impl` initialized all report entries to generic failure, but canceled and released later futures without updating their report fields. It also left the currently timed-out future queued if cancellation was still possible.
- **Decision:** Attempt to cancel the timed-out current future if pending. For later futures, record `TASKFORGE_ERR_CANCELLED` when cancellation succeeds; otherwise use a zero-time wait to record whether the future completed, failed, was canceled, or remains timed out. Preserve the underlying task error and completed result where available.
- **Regression:** Use a one-worker pool with a slow first task and queued second task; assert first item is timed out and second is canceled.
- **Trade-off:** Running callbacks cannot be interrupted safely by this API; they may finish after the map call returns.


## R-005 — IPC socket-path safety

- **Evidence:** POSIX/Linux `unlink(2)` removes a pathname that may name a regular file as well as a socket. See https://man7.org/linux/man-pages/man2/unlink.2.html and https://man7.org/linux/man-pages/man7/unix.7.html.
- **Decision:** Reject pre-existing paths; do not unlink after bind failure; on successful bind, remember the socket inode and remove only that same inode during cleanup. The explicit cleanup API rejects non-socket paths.
- **Trade-off:** stale socket files require explicit owner cleanup; the API named `server_stop` unlinks the pathname but cannot stop a server blocked in `accept()`, and this limitation is documented.

## R-006 — Handler error contract

- **Evidence:** Failure status plus error code zero could be returned to the client as success.
- **Decision:** Normalize a failed handler with an unset error code to `TASKFORGE_ERR_FAILED` on the server and client; add a handler regression test.


## R-007 — Avoid stale CI-run backlog during rapid PR updates

- **Question:** How should CI behave when several commits are pushed to one PR while runners are limited?
- **Primary reference:** GitHub Actions concurrency documentation: https://docs.github.com/en/actions/using-jobs/using-concurrency
- **Repository-specific evidence:** The prior `queue: max` plus `cancel-in-progress: false` configuration accumulated many stale runs for the same PR; a slow Valgrind install left older runs pending while new commits continued.
- **Decision:** Use a concurrency group per workflow and ref with `cancel-in-progress: true`. This cancels superseded runs on that same ref but still requires the latest revision to pass every quality gate. It does not cancel runs on other refs.
- **Validation:** Combined run [37965261107](https://github.com/rushmanthnalluri/task-forge/actions/runs/37965261107) completed all five gates successfully on code/test head `b6c9eacaa42e724d6dec0258ac7e5ec725f1304a`. A final run is required after the engineering-record edits.

## R-008 — Preserve map reports when submission fails partway through

- **Question:** How should a map report represent futures that were submitted successfully before a later submission was rejected?
- **Repository-specific evidence:** The submission-failure path waited for earlier futures but did not update their report entries, so successful results remained initialized as generic failures.
- **Decision:** Capture each earlier future's terminal status, error, and result while draining submitted work.
- **Regression:** A one-worker, capacity-one pool starts item 1, queues item 2, and uses concurrent immediate shutdown to reject item 3; the report must retain item 1's success and item 2's shutdown error.
- **Validation:** Covered by the combined code/test head that passed run `37965261107`; final docs-only revision still needs revalidation.
