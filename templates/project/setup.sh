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

case "${1:-}" in -h|--help) usage ;; esac

link="$(toy_get engine.link)"
ref="$(toy_get engine.ref)"
src="$(toy_get engine.source)"
src="${src:-$TOY_ENGINE_SOURCE_DEFAULT}"
mkdir -p "$PROJECT_DIR/.libs"

if [[ -n "$link" ]]; then
    [[ "$link" = /* ]] || link="$PROJECT_DIR/$link"
    # A vendored engine -- engine.link pointing at a git submodule inside this project -- is an
    # empty directory until that submodule is initialised, so a fresh clone of the project has
    # nothing to build. Populate it here, and only here: an initialised checkout may hold engine
    # work, and `git submodule update` on one would reset it to the recorded commit.
    if [[ ! -f "$link/CMakeLists.txt" && "$link" == "$PROJECT_DIR"/* ]] \
       && [[ -f "$PROJECT_DIR/.gitmodules" ]] && command -v git >/dev/null 2>&1 \
       && grep -q "path *= *${link#"$PROJECT_DIR"/}\$" "$PROJECT_DIR/.gitmodules"; then
        echo "[setup] initialising the engine submodule ${link#"$PROJECT_DIR"/}"
        git -C "$PROJECT_DIR" submodule update --init --recursive -- "$link" \
            || warn "could not initialise ${link#"$PROJECT_DIR"/}"
    fi
    if [[ ! -f "$link/CMakeLists.txt" || ! -d "$link/toyengine" ]]; then
        if [[ "$link" == "$PROJECT_DIR"/* ]]; then
            die "engine.link points at ${link#"$PROJECT_DIR"/}, which holds no toyengine checkout -- add one with 'toyhub vendor $PROJECT_DIR'"
        fi
        die "engine.link $link is not a toyengine checkout"
    fi
    if [[ -e "$ENGINE_DIR" && ! -L "$ENGINE_DIR" ]]; then
        die ".libs/toyengine is a clone; run ./clean.sh --all first to switch this project to link mode"
    fi
    ln -sfn "$link" "$ENGINE_DIR"
    if [[ "$link" == "$PROJECT_DIR"/* ]]; then
        echo "[setup] .libs/toyengine -> ${link#"$PROJECT_DIR"/} (vendored)"
    else
        echo "[setup] .libs/toyengine -> $link (linked)"
    fi
    # The coopa libraries are submodules of the engine. Same rule: init them only when they are
    # not there at all, never on a populated libs/.
    if [[ ! -f "$link/libs/libcoopa/CMakeLists.txt" && -e "$link/.git" ]] && command -v git >/dev/null 2>&1; then
        echo "[setup] initialising the engine's libs/ submodules"
        git -C "$link" submodule update --init --recursive || true
    fi
    [[ -f "$link/libs/libcoopa/CMakeLists.txt" ]] || warn "$link/libs is empty: run 'git submodule update --init --recursive' there"
else
    command -v git >/dev/null 2>&1 || die "git is needed to fetch the engine (or point engine.link in $(basename "$TOY_FILE") at a local checkout)"
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

# Agent notes: link the engine's .claude (its CLAUDE.md build/test rules and skills) in here
# too. Almost every change made while working in a project is an ENGINE change, reached through
# .libs/toyengine, so the engine's rules are the ones that apply -- and an agent whose working
# directory is the project never sees them otherwise, which is exactly how a 20-job build got
# run on a box whose rules say 4. Linked, not copied, so it tracks the engine checkout; the
# path is relative so it resolves the same on any machine. A project that needs rules of its
# own puts a CLAUDE.md at its root, which is read as well and is never touched by this.
if [[ -d "$ENGINE_DIR/.claude" ]]; then
    if [[ -L "$PROJECT_DIR/.claude" || ! -e "$PROJECT_DIR/.claude" ]]; then
        ln -sfn .libs/toyengine/.claude "$PROJECT_DIR/.claude"
    else
        warn ".claude exists and is not a symlink; leaving it alone (engine rules: $ENGINE_DIR/.claude)"
    fi
fi

# Toolchain report (cmake, a C++20 compiler, glslc) with this platform's install line. Only a
# report: setting a project up on a machine whose tools come later is fine -- build.sh is where
# a missing one is fatal.
require_toolchain warn
exit 0
