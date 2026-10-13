#!/usr/bin/env bash
# build.sh [release|debug] [-j N] [--target T] [--verbose] [-- <extra cmake configure args>]
#
# Builds build/<target> (the game) and build/<target>_editor; debug goes to build-debug/.
# The first run fetches or links the engine into .libs/ (setup.sh) -- expect several minutes;
# later runs only recompile what changed.
#
#   -j N, --jobs N   compile jobs (default: one per core, capped at one per 3 GB of RAM)
#   --target T       build only that CMake target, e.g. --target <target>_editor
#   --verbose        print every compile command
#   -- ...           passed to the cmake configure step, e.g. -- -DCMAKE_CXX_COMPILER=clang++
#
# Same on macOS and Linux; JOBS=N in the environment works like -j.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

config=Release
bdir="$PROJECT_DIR/build"
jobs=""
targets=()
verbose=()
while (( $# )); do
    case "$1" in
        -h|--help)      usage ;;
        debug|--debug)   config=Debug; bdir="$PROJECT_DIR/build-debug"; shift ;;
        release|--release) config=Release; shift ;;
        -j|--jobs)      jobs="${2:-}"; [[ -n "$jobs" ]] || die "$1 needs a number"; shift 2 ;;
        -j*)            jobs="${1#-j}"; shift ;;
        -t|--target)    [[ -n "${2:-}" ]] || die "$1 needs a target name"; targets+=(--target "$2"); shift 2 ;;
        -v|--verbose)   verbose=(--verbose); shift ;;
        --)             shift; break ;;
        *)              die "unknown argument '$1' (see ./build.sh --help)" ;;
    esac
done
extra=("$@")   # whatever followed --
[[ -n "$jobs" ]] || jobs="$(build_jobs)"

require_toolchain
[[ -f "$ENGINE_DIR/CMakeLists.txt" ]] || "$PROJECT_DIR/setup.sh"
[[ -f "$ENGINE_DIR/CMakeLists.txt" ]] || die ".libs/toyengine is still missing -- run ./setup.sh"

echo "[build] $TOY_NAME ($config, $jobs jobs) -> $bdir"
cmake -S "$PROJECT_DIR" -B "$bdir" -DCMAKE_BUILD_TYPE="$config" ${extra[@]+"${extra[@]}"}
cmake --build "$bdir" -j "$jobs" ${targets[@]+"${targets[@]}"} ${verbose[@]+"${verbose[@]}"}
if (( ${#targets[@]} )); then
    echo "[build] done"
else
    echo "[build] $bdir/$TOY_TARGET and $bdir/${TOY_TARGET}_editor"
fi
