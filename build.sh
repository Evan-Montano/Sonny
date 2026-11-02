
#!/usr/bin/env bash

set -Eeuo pipefail

trap 'status=$?; echo; printf "BUILD FAILED (exit %s, line %s)\n" "$status" "$LINENO"; exit "$status"' ERR

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "========================================"
echo "           SONNY BUILD"
echo "========================================"

echo
echo "=== Checking Dependencies ==="

if [[ ! -d "$HOME/.local/faiss" ]]; then
    echo "ERROR: FAISS installation not found at ~/.local/faiss"
    exit 1
fi

if [[ ! -x "$PROJECT_DIR/protobuf/install/bin/protoc" ]]; then
    echo "ERROR: Protobuf installation not found."
    echo "Run ./full-build.sh first."
    exit 1
fi

echo "FAISS installation found."
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
echo "       BUILD AND TESTS SUCCESSFUL"
echo "========================================"
