#!/usr/bin/env bash
set -e

echo "=== Installing system dependencies for CAML ==="
sudo apt update
sudo apt install -y libzstd-dev libssl-dev libsodium-dev build-essential cmake pkg-config

echo "=== Building CAML C++ library & binary ==="
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

cmake -B build -S .
cmake --build build -j$(nproc)

echo "=== CAML build complete ==="
