# shellcheck shell=bash
# .toyenv.sh -- shared by this project's scripts (sourced, not run). Kept in the project, not in
# .libs/, because setup.sh needs it before .libs/toyengine exists.

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

ncpu() { sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4; }
