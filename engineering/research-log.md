# TaskForge Research Log

**Updated:** 2026-10-09 22:31 IST

## R-001 — POSIX unlink semantics and pathname socket lifecycle

**Question:** Is it safe for a library API to call `unlink(path)` before binding a Unix-domain socket?

**Primary references:**
- Linux man-pages, `unlink(2)`: https://man7.org/linux/man-pages/man2/unlink.2.html
- Linux man-pages, `unix(7)`: https://man7.org/linux/man-pages/man7/unix.7.html
- Linux man-pages, `bind(2)`: https://www.man7.org/linux/man-pages/man2/bind.2.html

**Evidence:** `unlink(2)` removes the directory entry named by a pathname; this applies to regular files as well as socket paths. `unix(7)` notes that filesystem pathname sockets are explicitly removed by callers when no longer needed. Therefore, a server cannot safely assume an arbitrary existing path is a stale socket.

**Decision:** Do not automatically delete an existing path. Reject existing paths before bind; on failure, do not unlink anything. After successful bind, record the created socket's device/inode and only remove that same socket entry on cleanup. `taskforge_ipc_server_stop` rejects non-socket paths.

**Trade-offs:** Existing stale socket paths now require explicit cleanup by the owner. This is preferable to silently deleting arbitrary files or unconditionally removing another server's path. Inode checks reduce accidental cleanup of a replaced path; they do not replace a broader threat model for hostile directory manipulation.

**Acceptance criteria:** A regular file at the requested path remains byte-for-byte unchanged after failed server start/stop; normal socket lifecycle and IPC tests remain green.

## R-002 — Stream sockets and response framing

**Question:** Can a line reader be used for a length-prefixed response body?

**Primary reference:** Linux man-pages, `unix(7)`: https://man7.org/linux/man-pages/man7/unix.7.html

**Repository evidence:** TaskForge response headers advertise body length. Treating the body as a line makes embedded newline characters indistinguishable from a protocol delimiter.

**Decision:** Read exactly the advertised number of bytes, then validate the protocol's trailing newline delimiter. Reject lengths that exceed configured payload/result limits. Add a multiline response regression test.

**Acceptance criteria:** The response `first line\nsecond line` round-trips exactly; malformed delimiters and invalid lengths are rejected. One monotonic deadline spans response-header and response-body reads, so slow trickle traffic cannot renew the timeout for every byte.

## R-003 — Total response deadline\n\n**Question:** Should a slow stream be allowed to reset the client timeout for each byte?\n\n**Repository evidence:** the prior reader called `poll()` with the full timeout before each byte, so a peer could stretch a nominal timeout across an arbitrarily long header/body.\n\n**Decision:** Use `CLOCK_MONOTONIC` to establish one deadline after the request is sent; header and body reads share it. The server's request-line reader also uses a total deadline. Add a slow-trickle response regression test.\n\n**Acceptance criteria:** A response body that trickles bytes past the deadline returns timeout promptly.\n\n## R-004 — Error contract consistency

**Question:** What happens when a handler reports failure but leaves its error code at zero?

**Repository evidence:** The server previously encoded `ok = false, error = 0`, and the client returned the error value directly, which could be zero.

**Decision:** Normalize failed handlers with an unset error to `TASKFORGE_ERR_FAILED` on both server and client paths. Add a regression test.

**Acceptance criteria:** A failing handler with no explicit error cannot be observed as successful.

## R-005 — CI evidence and race findings

**Evidence:**
- PR #3 workflow run #172: https://github.com/rushmanthnalluri/task-forge/actions/runs/37962540470 — build/tests/CLI, ASan+UBSan, Valgrind, and million-task soak passed; TSan failed with a race report in pool initialization on the older base revision.
- Current `main` workflow run #176: https://github.com/rushmanthnalluri/task-forge/actions/runs/37963052958 — all five jobs completed successfully, including TSan.

**Decision:** Keep historical failures in the record and require current-revision CI to pass; never infer that a newer code revision is covered by an older workflow run.

## Deferred research

- Review queue/future ownership against cancellation and cleanup callback invariants.
- Review parser integer conversion and malformed-input behavior.
- Review whether benchmark reports accurately distinguish measured results from environment-specific observations.
- License selection requires an explicit repository-owner decision; no license is inferred from public visibility.
