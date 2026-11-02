
#!/usr/bin/env bash

set -Eeuo pipefail

trap 'status=$?; echo; printf "BUILD FAILED (exit %s, line %s)\n" "$status" "$LINENO"; exit "$status"' ERR

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "========================================"
echo "          FULL SONNY BUILD"
echo "========================================"

echo
echo "=== Cleaning Sonny build ==="
rm -rf build

echo
echo "=== Cleaning FAISS build ==="
rm -rf faiss/build

echo
echo "=== Cleaning Protobuf build and install ==="
rm -rf protobuf/build
rm -rf protobuf/install

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
echo "=== Configuring Protobuf ==="
cmake -S protobuf \
    -B protobuf/build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PROJECT_DIR/protobuf/install" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_STANDARD=17 \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF

echo
echo "=== Building Protobuf ==="
cmake --build protobuf/build --parallel 4

echo
echo "=== Installing Protobuf ==="
cmake --install protobuf/build

echo
echo "=== Verifying Protobuf ==="
protobuf/install/bin/protoc --version

echo
echo "=== Configuring Sonny ==="
cmake -S . \
    -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$HOME/.local/faiss;$PROJECT_DIR/protobuf/install"

echo
echo "=== Building Sonny ==="
cmake --build build

echo
echo "=== Running Tests ==="
ctest --test-dir build --output-on-failure

echo
echo "========================================"
echo "    FULL BUILD AND TESTS SUCCESSFUL"
echo "========================================"
