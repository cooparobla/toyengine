#!/usr/bin/env bash
set -e

echo "=== Uninstalling CAML dependencies and build artifacts ==="
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -d "$SCRIPT_DIR/build" ]; then
    echo "Removing build directory..."
    rm -rf "$SCRIPT_DIR/build"
fi

echo "Removing system dependencies (libzstd-dev, libssl-dev, libsodium-dev)..."
sudo apt purge -y libzstd-dev libssl-dev libsodium-dev
sudo apt autoremove -y

echo "=== CAML uninstalled successfully ==="
