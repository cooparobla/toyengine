#!/usr/bin/env bash
# build.sh [release|debug] [-- <extra cmake configure args>]
#   Builds build/<target> (the game) and build/<target>_editor; debug goes to build-debug/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

config=Release
bdir="$PROJECT_DIR/build"
case "${1:-}" in
    debug)   config=Debug; bdir="$PROJECT_DIR/build-debug"; shift ;;
    release) shift ;;
esac
[[ "${1:-}" == "--" ]] && shift
extra=("$@")

[[ -f "$ENGINE_DIR/CMakeLists.txt" ]] || "$PROJECT_DIR/setup.sh"

cmake -S "$PROJECT_DIR" -B "$bdir" -DCMAKE_BUILD_TYPE="$config" ${extra[@]+"${extra[@]}"}
cmake --build "$bdir" -j "$(ncpu)"
echo "[build] $bdir/$TOY_TARGET and $bdir/${TOY_TARGET}_editor"
