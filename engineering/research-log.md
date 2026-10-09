# Research log

**Updated:** 2026-10-09 22:32:41 IST

## R-001 — POSIX thread lifecycle and lock ordering
- **Question:** Can a pool resize hold a mutex while joining a worker whose callback may call back into the pool?
- **Primary references:**
  - POSIX `pthread_join`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_join.html
  - POSIX mutex locking: https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html
- **Repository-specific evidence:** `taskforge_pool_resize` held `resize_mutex` across `pthread_join`; `taskforge_pool_worker_count` acquired that same mutex. A worker callback calling the count API could wait for the resizer while the resizer waited for that callback to return.
- **Decision:** Make worker-count reads atomic; keep worker-local deque mutexes alive during shrink/re-growth to avoid stats racing with destruction. This is a narrow change without introducing another condition variable or changing the public API.
- **Risk / verification:** Worker storage is fixed-capacity. Pool destruction still must not race with any other pool operation, as documented. CI must validate the new regression under TSan and normal tests.
- **Outcome:** Implemented on mission branch in commit `ce43a233a47d3ae9acf71b531b5257129764cb09`; CI result pending.

## R-002 — IPC framing
- **Question:** Does the current line protocol safely represent the byte lengths exposed by the API?
- **Primary references:**
  - POSIX `recv`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/recv.html
  - POSIX `send`: https://pubs.opengroup.org/onlinepubs/9799919799/functions/send.html
- **Repository-specific evidence:** Client API accepts pointer plus byte length, and response header carries a body length, but both request and response bodies are read using newline-delimited logic. The server parses but does not validate the request protocol version.
- **Options:** (A) Restrict v1 to newline-free text and document that limitation; (B) introduce explicit length framing and a protocol-version transition. Option B better matches the existing length-aware API, but needs compatibility analysis and exact-length/timeout tests.
- **Decision:** Keep open until compatibility and framing behavior can be changed with regression coverage. No protocol changes made yet.

## R-003 — Producer-ticket cancellation under allocation failure
- **Question:** Can queue fairness bookkeeping fail without losing liveness?
- **Repository-specific evidence:** `cancel_ticket` allocates a dynamically growing canceled-ticket array. `abandon_ticket` ignores allocation failure, potentially leaving a ticket gap that prevents `producer_turn` from advancing.
- **Options:** Remove ticket-based FIFO admission, use allocation-free bounded bookkeeping, or make the queue fail closed and deterministically resolve all waiting producers.
- **Decision:** Open. Do not replace the queue admission model without tests that define producer fairness and ensure shutdown wakes all waiters.
