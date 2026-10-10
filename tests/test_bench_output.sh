#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

set +e
(cd "$tmpdir" && "$repo_root/bin/bench_scaling" 1 >bench.log 2>&1)
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
    echo "Benchmark unexpectedly succeeded without its CSV output directory." >&2
    exit 1
fi

if ! grep -q "Failed to open benchmark CSV" "$tmpdir/bench.log"; then
    cat "$tmpdir/bench.log" >&2
    echo "Benchmark failed without an actionable CSV error." >&2
    exit 1
fi

echo "[PASS] Benchmark reports CSV output failure."
