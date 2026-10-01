#!/usr/bin/env bash

set -e

trap 'echo; echo "BUILD FAILED"; exit 1' ERR

cd "$(dirname "$0")"

echo "========================================"
echo "       FULL SONNY BUILD"
echo "========================================"

echo
echo "=== Cleaning Sonny build ==="
rm -rf build

echo
echo "=== Cleaning FAISS build ==="
rm -rf faiss/build

echo
echo "=== Configuring FAISS ==="
cmake -S faiss \
    -B faiss/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local/faiss" \
    -DFAISS_ENABLE_GPU=OFF \
    -DFAISS_ENABLE_PYTHON=OFF \
    -DBUILD_TESTING=OFF

echo
echo "=== Building FAISS ==="
cmake --build faiss/build --target faiss

echo
echo "=== Installing FAISS ==="
cmake --install faiss/build

echo
echo "=== Configuring Sonny ==="
cmake -S . \
    -B build \
    -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/.local/faiss"

echo
echo "=== Building Sonny ==="
cmake --build build

echo
echo "=== Running Tests ==="
ctest --test-dir build --output-on-failure

echo
echo "========================================"
echo "   FULL BUILD AND TESTS SUCCESSFUL"
echo "========================================"