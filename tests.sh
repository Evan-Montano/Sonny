#!/usr/bin/env bash

set -Eeuo pipefail

trap 'status=$?; echo; printf "TESTS FAILED (exit %s, line %s)\n" "$status" "$LINENO"; exit "$status"' ERR

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "========================================"
echo "           SONNY TESTS"
echo "========================================"

echo
echo "=== Running Tests ==="
ctest --test-dir build --output-on-failure

echo
echo "========================================"
echo "           TESTS SUCCESSFUL"
echo "========================================"