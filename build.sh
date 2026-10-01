#!/usr/bin/env bash

set -e

trap 'echo; echo "BUILD FAILED"; exit 1' ERR

cd "$(dirname "$0")"

echo "=== Configuring Sonny ==="
cmake -S . \
    -B build \
    -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/.local/faiss"

echo "=== Building Sonny ==="

cmake --build build

echo
echo "=== Running Tests ==="

ctest --test-dir build --output-on-failure

echo
echo "=== Build and tests successful ==="