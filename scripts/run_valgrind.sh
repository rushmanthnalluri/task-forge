#!/usr/bin/env bash
set -e

echo "================================================="
echo "  TaskForge Valgrind Leak-Check & Memory Verifier"
echo "================================================="

cd "$(dirname "$0")/.."

make -j$(nproc) all

if ! command -v valgrind >/dev/null; then
  echo "Valgrind is not installed." >&2
  exit 2
fi

for test in ./bin/test_*; do
  [ -x "$test" ] || continue
  valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 "$test"
done

echo "================================================="
echo "  VALGRIND: all discovered tests completed under Memcheck."
echo "================================================="
