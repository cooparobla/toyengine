#!/usr/bin/env bash
# setup_macos.sh -- install toyengine's build/run dependencies on macOS via Homebrew.
#
# Vulkan on macOS runs through MoltenVK (Vulkan-on-Metal). This installs the
# Khronos loader, MoltenVK, headers, validation layers and glslc (shaderc), plus
# GLFW. gfxcoopa finds the Homebrew loader at runtime on its own (see
# libs/gfxcoopa/gfxcoopa/util/volk_init.h), so no DYLD_* variables are needed.
#
# Linux is unaffected: nothing in the build calls this script.
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "setup_macos.sh: this script is for macOS only." >&2
    exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
    echo "setup_macos.sh: Homebrew is required -- see https://brew.sh" >&2
    exit 1
fi

brew install cmake glfw vulkan-loader vulkan-headers molten-vk vulkan-validationlayers shaderc

cd "$(dirname "$0")/.."
git submodule update --init --recursive

cat <<'EOF'

Dependencies installed. Build and run with:

    cmake -B build && cmake --build build -j
    ./build/toyengine [scene]            # e.g. ./build/toyengine terrain_demo
    ctest --test-dir build -j4

For a debug build with Vulkan validation enabled:

    cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug -j
EOF
