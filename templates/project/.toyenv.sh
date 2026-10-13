# shellcheck shell=bash
# .toyenv.sh -- shared by this project's scripts (sourced, not run). Kept in the project, not in
# .libs/, because setup.sh needs it before .libs/toyengine exists.
#
# The scripts run on macOS and Linux and stay inside what both ship: bash 3.2 (macOS's
# /bin/bash), POSIX awk/sed, and no GNU-only flags. Anything platform-specific lives in a
# helper here -- is_macos, ncpu, mem_gb, require_toolchain -- so the scripts themselves read the
# same on either system.

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOY_FILE="$(ls "$PROJECT_DIR"/*.toy 2>/dev/null | head -n 1 || true)"
ENGINE_DIR="$PROJECT_DIR/.libs/toyengine"
TOY_ENGINE_SOURCE_DEFAULT="git@github.com:cooparobla/toyengine.git"

# A double-clicked app / GUI launch has a bare PATH: make Homebrew's cmake/glslc visible.
for _p in /opt/homebrew/bin /usr/local/bin; do
    if [[ -d "$_p" && ":$PATH:" != *":$_p:"* ]]; then PATH="$_p:$PATH"; fi
done
export PATH

die()  { echo "error: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }

# usage -- the leading comment block of the calling script, as its --help text.
usage() { sed -n '2,/^set -/p' "$0" | sed -e '/^set -/d' -e 's/^# \{0,1\}//'; exit 0; }

[[ -n "$TOY_FILE" ]] || die "no .toy project file in $PROJECT_DIR"

# toy_get <key> | <section>.<key> -- one scalar from the .toy file (flat YAML only).
toy_get() {
    awk -v want="$1" '
        /^[[:space:]]*#/ { next }
        {
            line = $0
            sub(/[[:space:]]+#.*$/, "", line)
            if (line ~ /^[[:space:]]*$/) next
            match(line, /^ */); ind = RLENGTH
            body = substr(line, ind + 1)
            c = index(body, ":"); if (c == 0) next
            key = substr(body, 1, c - 1)
            val = substr(body, c + 1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
            gsub(/^["\047]|["\047]$/, "", val)
            if (ind == 0) { sect = key; full = key } else { full = sect "." key }
            if (full == want) { print val; exit }
        }' "$TOY_FILE"
}

TOY_NAME="$(basename "$PROJECT_DIR")"   # a project is named after its folder
TOY_TARGET="$(toy_get target)"
[[ -n "$TOY_TARGET" ]] || die "$TOY_FILE has no target:"

# --- platform ---------------------------------------------------------------------------------

is_macos() { [[ "$(uname -s)" == Darwin ]]; }

ncpu() { sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4; }

# mem_gb -- installed RAM in whole GB, 0 when it cannot be read.
mem_gb() {
    local bytes kb
    if is_macos; then
        bytes="$(sysctl -n hw.memsize 2>/dev/null || echo 0)"
        echo $(( bytes / 1073741824 ))
    else
        kb="$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo 2>/dev/null || echo 0)"
        echo $(( ${kb:-0} / 1048576 ))
    fi
}

# build_jobs -- compile jobs for cmake --build: $JOBS if set, else one per core but no more than
# one per 3 GB of RAM. The engine's heaviest translation units peak well over 1 GB and a link
# needs more, so a bare core count (20 jobs on a 16 GB laptop) swaps, stalls the desktop or gets
# the compiler OOM-killed mid-build.
build_jobs() {
    if [[ -n "${JOBS:-}" ]]; then echo "$JOBS"; return 0; fi
    local cores mem cap
    cores="$(ncpu)"
    mem="$(mem_gb)"
    if (( mem <= 0 )); then cap=4; else cap=$(( mem / 3 )); fi
    if (( cap < 1 )); then cap=1; fi
    if (( cap > cores )); then cap="$cores"; fi
    echo "$cap"
}

# require_toolchain [warn] -- what a build needs, with the install line for this platform. A
# missing tool is fatal unless "warn" is passed (setup.sh only reports it; the build is what
# cannot go on without one). glslc only ever warns: configure warns too, and the build then
# simply has no compiled shaders.
require_toolchain() {
    local mode="${1:-strict}" missing="" soft="" t
    for t in cmake; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
    command -v c++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 \
        || missing="$missing c++"
    command -v glslc >/dev/null 2>&1 || soft="$soft glslc"
    [[ -z "$missing$soft" ]] && return 0
    local hint
    if is_macos; then
        hint="$ENGINE_DIR/tools/setup_macos.sh   (Homebrew: cmake glfw vulkan-loader vulkan-headers molten-vk shaderc)"
    else
        hint="sudo apt install cmake g++ pkg-config libvulkan-dev glslc libglfw3-dev libssl-dev libzstd-dev"
    fi
    [[ -n "$soft" ]] && warn "missing$soft -- the build will have no compiled shaders"
    if [[ -n "$missing" ]]; then
        if [[ "$mode" == warn ]]; then warn "missing$missing"; else die "missing$missing -- install with: $hint"; fi
    fi
    warn "install with: $hint"
    return 0
}

# ensure_binary <path> -- builds the project when <path> is not there yet (the first ./run.sh,
# ./editor.sh or ./package.sh in a fresh clone), unless NO_BUILD=1.
ensure_binary() {
    [[ -x "$1" ]] && return 0
    [[ "${NO_BUILD:-0}" == 1 ]] && die "$1 not built -- run ./build.sh"
    echo "[$(basename "$0" .sh)] $(basename "$1") is not built yet -- building it first"
    # A BUILD_DIR named like build-debug asks for the Debug tree build.sh writes there.
    case "$(basename "${BUILD_DIR:-build}")" in
        *debug*) "$PROJECT_DIR/build.sh" debug ;;
        *)       "$PROJECT_DIR/build.sh" ;;
    esac || die "build failed"
    [[ -x "$1" ]] || die "$1 still missing after a build -- is $TOY_FILE's target: right?"
}

# ensure_assets -- a new project is just a .toy + scripts, and the game needs an assets/ tree to
# load (config.yaml names the scene it starts in). Lays it down through the editor's own
# --new-project, the same code its first open runs, so there is one definition of what a starter
# project holds: config.yaml, scenes/main, the cube/plane/sphere meshes and a default material.
# No window and one frame long. Existing files are never overwritten.
ensure_assets() {
    [[ -f "$PROJECT_DIR/assets/config.yaml" ]] && return 0
    local ed log
    ed="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
    [[ "$ed" = /* ]] || ed="$PROJECT_DIR/$ed"
    ensure_binary "$ed"
    echo "[$(basename "$0" .sh)] creating assets/ (config.yaml, a starter scene and meshes)"
    mkdir -p "$PROJECT_DIR/.toyeditor"
    log="$PROJECT_DIR/.toyeditor/init.log"
    HEADLESS=1 MAX_FRAMES=1 "$ed" --new-project "$PROJECT_DIR" > "$log" 2>&1 \
        || die "could not create assets/ (see .toyeditor/init.log); ./editor.sh creates it too"
    [[ -f "$PROJECT_DIR/assets/config.yaml" ]] \
        || die "assets/config.yaml still missing (see .toyeditor/init.log)"
}
