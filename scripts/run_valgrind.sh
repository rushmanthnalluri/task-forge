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
  case "$test" in
    ./bin/test_million_soak) continue ;;
  esac
  valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 "$test"
done

echo "================================================="
echo "  VALGRIND: regular regression tests completed under Memcheck; the one-million-task soak is covered separately by CI sanitizer/soak stages."
echo "================================================="
