#!/usr/bin/env bash
# build.sh -- build toyengine's binaries with the job count this machine survives.
#
#   ./build.sh                 # the apps: toyengine, toyengine_editor, toyengine_hub
#   ./build.sh editor          # one shorthand (see the table below)
#   ./build.sh tests           # both test runners
#   ./build.sh all             # the apps and the test runners
#   ./build.sh toyengine_icon  # any CMake target name, as many as you like
#   ./build.sh -j 8 engine     # override the job count (JOBS=8 ./build.sh works too)
#
# Shorthands, so the common targets need no typing:
#
#   game    -> toyengine                 engine  -> toyengine_engine (compile only, no link)
#   editor  -> toyengine_editor          lib     -> toyengine_editor_lib
#   hub     -> toyengine_hub             shaders -> toyengine_shaders uicoopa_shaders
#   icon    -> toyengine_icon            tests   -> toyengine_tests toyengine_editor_tests
#
# Four jobs by default, deliberately: at ~2 GB per compiler this 16 GB machine OOM-kills the
# session somewhere above that (the same reason .vscode/settings.json pins cmake.parallelJobs).
#
# Builds ONLY the targets named -- `cmake --build build` with no target builds every
# executable, which is minutes of work nobody asked for. Nothing here cleans or reconfigures:
# a new .cpp is picked up on its own (GLOB_RECURSE CONFIGURE_DEPENDS), and build/ is only
# configured when it does not exist yet.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

jobs="${JOBS:-4}"
targets=()

while (($#)); do
    case "$1" in
        -j|--jobs)   jobs="${2:?-j needs a number}"; shift 2 ;;
        -j*)         jobs="${1#-j}"; shift ;;
        -h|--help)   sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//;$d'; exit 0 ;;
        game)        targets+=(toyengine); shift ;;
        editor)      targets+=(toyengine_editor); shift ;;
        hub)         targets+=(toyengine_hub); shift ;;
        icon)        targets+=(toyengine_icon); shift ;;
        engine)      targets+=(toyengine_engine); shift ;;
        lib)         targets+=(toyengine_editor_lib); shift ;;
        shaders)     targets+=(toyengine_shaders uicoopa_shaders); shift ;;
        tests)       targets+=(toyengine_tests toyengine_editor_tests); shift ;;
        apps)        targets+=(toyengine toyengine_editor toyengine_hub); shift ;;
        all)         targets+=(toyengine toyengine_editor toyengine_hub
                               toyengine_tests toyengine_editor_tests); shift ;;
        -*)          echo "build.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
        *)           targets+=("$1"); shift ;;
    esac
done

((${#targets[@]})) || targets=(toyengine toyengine_editor toyengine_hub)

# A fresh clone has no build/ yet. An existing one is left exactly as it is -- reconfiguring
# "to be safe" only costs time.
if [[ ! -f build/CMakeCache.txt ]]; then
    echo "[build.sh] configuring build/ (first run)"
    cmake -B build
fi

echo "[build.sh] ${targets[*]}  (-j${jobs})"
start=$SECONDS
cmake --build build --target "${targets[@]}" "-j${jobs}"
echo "[build.sh] done in $((SECONDS - start))s"
