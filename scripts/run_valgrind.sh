#!/usr/bin/env bash
set -e

echo "================================================="
echo "  TaskForge Valgrind Leak-Check & Memory Verifier"
echo "================================================="

cd "$(dirname "$0")/.."

make -j$(nproc) all

valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./bin/test_futures
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./bin/test_shutdown
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./bin/test_bounded_queue
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./bin/test_map

echo "================================================="
echo "  VALGRIND: ZERO MEMORY LEAKS DETECTED!"
echo "================================================="
