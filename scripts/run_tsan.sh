#!/usr/bin/env bash
set -e

echo "================================================="
echo "  TaskForge ThreadSanitizer (TSan) Race Verifier"
echo "================================================="

cd "$(dirname "$0")/.."

# On Linux 6.x / Ubuntu 24.04, set vm.mmap_rnd_bits=28 to prevent TSan address mapping conflicts
sysctl -w vm.mmap_rnd_bits=28 2>/dev/null || true

make tsan

echo "================================================="
echo "  TSAN: test suite completed under ThreadSanitizer."
echo "================================================="
