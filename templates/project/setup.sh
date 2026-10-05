#!/usr/bin/env bash
# setup.sh -- puts this project's engine in .libs/toyengine, as the .toy file's engine: block says.
#
#   engine.link set   -> .libs/toyengine is a symlink to that toyengine checkout (engine dev:
#                        its uncommitted edits, libs/ included, build straight into this project)
#   otherwise         -> a git clone of engine.source checked out at engine.ref, submodules
#                        initialised. Re-running fetches and moves to a changed ref.
# Idempotent; build.sh runs it when .libs/toyengine is missing.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

link="$(toy_get engine.link)"
ref="$(toy_get engine.ref)"
src="$(toy_get engine.source)"
src="${src:-$TOY_ENGINE_SOURCE_DEFAULT}"
mkdir -p "$PROJECT_DIR/.libs"

if [[ -n "$link" ]]; then
    [[ "$link" = /* ]] || link="$PROJECT_DIR/$link"
    [[ -f "$link/CMakeLists.txt" && -d "$link/toyengine" ]] || die "engine.link $link is not a toyengine checkout"
    if [[ -e "$ENGINE_DIR" && ! -L "$ENGINE_DIR" ]]; then
        die ".libs/toyengine is a clone; run ./clean.sh --all first to switch this project to link mode"
    fi
    ln -sfn "$link" "$ENGINE_DIR"
    echo "[setup] .libs/toyengine -> $link (linked)"
    [[ -f "$link/libs/libcoopa/CMakeLists.txt" ]] || warn "$link/libs is empty: run 'git submodule update --init --recursive' there"
else
    if [[ -L "$ENGINE_DIR" ]]; then rm "$ENGINE_DIR"; fi
    ref="${ref:-main}"
    if [[ ! -e "$ENGINE_DIR/.git" ]]; then
        echo "[setup] cloning $src"
        git clone --quiet "$src" "$ENGINE_DIR"
    fi
    cd "$ENGINE_DIR"
    if [[ -n "$(git status --porcelain --untracked-files=no --ignore-submodules=dirty)" ]]; then
        warn ".libs/toyengine has local changes; leaving it at $(git rev-parse --short HEAD) (wanted $ref)"
    else
        git fetch --quiet --tags origin || warn "fetch failed (offline?); using what is already cloned"
        if git show-ref --verify --quiet "refs/remotes/origin/$ref"; then
            # A branch: track it and fast-forward.
            git checkout --quiet -B "$ref" "origin/$ref"
        else
            git checkout --quiet --detach "$ref" || die "engine.ref '$ref' not found in $src (pushed?)"
        fi
        echo "[setup] .libs/toyengine at $ref ($(git rev-parse --short HEAD))"
    fi
    git submodule update --init --recursive --quiet
fi

# Toolchain check (macOS: tools/setup_macos.sh installs everything via Homebrew).
missing=()
for t in cmake glslc; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
if (( ${#missing[@]} )); then
    warn "missing: ${missing[*]}"
    if [[ "$(uname -s)" == Darwin ]]; then warn "install with: $ENGINE_DIR/tools/setup_macos.sh"; fi
fi
exit 0
