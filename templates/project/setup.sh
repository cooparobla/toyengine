#!/usr/bin/env bash
# setup.sh -- puts this project's engine in .libs/toyengine, as the .toy file's engine: block says.
#
#   engine.link absolute   -> .libs/toyengine symlinks that shared toyengine checkout (engine
#                             dev: its uncommitted edits, libs/ included, build straight into
#                             this project). engine.mode: linked
#   engine.link relative   -> the project's OWN engine, inside the project (engine/ by default):
#                             a git worktree of a checkout on this machine (engine.mode:
#                             worktree, on engine.branch) or its own clone (engine.mode: own).
#                             .libs/toyengine symlinks it. Missing -- a fresh clone of the
#                             project, which carries the engine: block but not the engine --
#                             is cloned from engine.source here at engine.branch or engine.ref.
#   neither                -> a git clone of engine.source in .libs/toyengine, checked out at
#                             engine.ref. Re-running fetches and moves to a changed ref.
#
# `toyhub link|worktree|own <dir>` is what switches a project between the modes.
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
    # An engine the project keeps inside itself is a git checkout of its own, so it is not part
    # of the project's history and a fresh clone of the project arrives without it: clone it
    # here, at the branch the project's engine work is on if there is one. Only when it is
    # missing -- a checkout that is there may hold engine work, and re-fetching one would be a
    # way to lose it. `toyhub worktree` is what makes the worktree form, which needs a toyengine
    # checkout on this machine to branch from; a clone is what a machine without one gets.
    if [[ ! -f "$link/CMakeLists.txt" && "$link" == "$PROJECT_DIR"/* ]]; then
        command -v git >/dev/null 2>&1 || die "git is needed to fetch this project's engine into ${link#"$PROJECT_DIR"/}"
        if [[ -e "$PROJECT_DIR/.gitmodules" ]] && grep -q "path *= *${link#"$PROJECT_DIR"/}\$" "$PROJECT_DIR/.gitmodules"; then
            # An engine an older toyhub vendored as a git submodule.
            echo "[setup] initialising the engine submodule ${link#"$PROJECT_DIR"/}"
            git -C "$PROJECT_DIR" submodule update --init -- "$link" || warn "could not initialise ${link#"$PROJECT_DIR"/}"
        else
            branch="$(toy_get engine.branch)"
            echo "[setup] cloning $src into ${link#"$PROJECT_DIR"/} (this project's own engine)"
            rmdir "$link" 2>/dev/null || true
            git clone --quiet "$src" "$link" || die "could not clone $src into ${link#"$PROJECT_DIR"/}"
            at="${branch:-$ref}"
            if [[ -n "$at" ]]; then
                if git -C "$link" show-ref --verify --quiet "refs/remotes/origin/$at"; then
                    git -C "$link" checkout --quiet -B "$at" "origin/$at"
                elif git -C "$link" rev-parse --verify --quiet "$at^{commit}" >/dev/null; then
                    git -C "$link" checkout --quiet --detach "$at"
                else
                    warn "engine '$at' is not in $src (pushed?); ${link#"$PROJECT_DIR"/} stays on $(git -C "$link" rev-parse --short HEAD)"
                fi
            fi
        fi
    fi
    if [[ ! -f "$link/CMakeLists.txt" || ! -d "$link/src/toyengine" ]]; then
        if [[ "$link" == "$PROJECT_DIR"/* ]]; then
            die "engine.link points at ${link#"$PROJECT_DIR"/}, which holds no toyengine checkout -- make one with 'toyhub worktree $PROJECT_DIR' (a branch of a local checkout) or 'toyhub own $PROJECT_DIR' (its own clone)"
        fi
        die "engine.link $link is not a toyengine checkout"
    fi
    if [[ -e "$ENGINE_DIR" && ! -L "$ENGINE_DIR" ]]; then
        die ".libs/toyengine is a clone; run ./clean.sh --all first to switch this project to link mode"
    fi
    ln -sfn "$link" "$ENGINE_DIR"
    if [[ "$link" == "$PROJECT_DIR"/* ]]; then
        mode="$(toy_get engine.mode)"
        echo "[setup] .libs/toyengine -> ${link#"$PROJECT_DIR"/} (this project's own engine${mode:+, $mode})"
    else
        echo "[setup] .libs/toyengine -> $link (linked)"
    fi
    # The coopa libraries are committed inside the engine checkout, so they arrive with it.
    [[ -f "$link/src/libs/libcoopa/CMakeLists.txt" ]] || warn "$link/src/libs is empty: that engine checkout is incomplete"
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
    # The coopa libraries are committed inside the engine, so the clone is complete as it stands.
    [[ -f "$ENGINE_DIR/src/libs/libcoopa/CMakeLists.txt" ]] || warn ".libs/toyengine/src/libs is empty: that clone is incomplete"
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

# assets/config.yaml: seeded from the engine's own, which is where the tuned render settings
# live (aa_mode, auto-exposure, bloom, contact shadows, the quality tiers). The engine reads
# exactly ONE path for it -- <project>/assets/config.yaml, no search across asset roots -- and
# falls back to hardcoded defaults when it is missing, so a project without one renders visibly
# unlike the engine. The EDITOR already seeds it (Project::default_config()), but a project made
# with `toyhub new` never goes through that, which is how one ended up on bare defaults.
#
# Only the three keys that are the project's own identity are changed, exactly as
# Project::default_config() changes them. Never overwrites an existing file: a project is free to
# tune its own look, and two of them already have.
if [[ -f "$ENGINE_DIR/assets/config.yaml" && ! -f "$PROJECT_DIR/assets/config.yaml" ]]; then
    mkdir -p "$PROJECT_DIR/assets"
    cp "$ENGINE_DIR/assets/config.yaml" "$PROJECT_DIR/assets/config.yaml"
    # Each of the three is a single-line flow mapping (`window: { height: 1080, title: ... }`),
    # so every substitution is anchored to its own key's line rather than matching the token
    # anywhere in the file.
    sed -i.bak \
        -e 's|^scene: { default_scene: .* }$|scene: { default_scene: assets/scenes/main/scene.yaml }|' \
        -e '/^output: {/ s|save_on_exit: true|save_on_exit: false|' \
        -e "/^window: {/ s|title: toyengine|title: $TOY_NAME|" \
        "$PROJECT_DIR/assets/config.yaml"
    rm -f "$PROJECT_DIR/assets/config.yaml.bak"
    echo "[setup] assets/config.yaml seeded from the engine's (title: $TOY_NAME, default scene: assets/scenes/main/scene.yaml)"
fi

# Toolchain report (cmake, a C++20 compiler, glslc) with this platform's install line. Only a
# report: setting a project up on a machine whose tools come later is fine -- build.sh is where
# a missing one is fatal.
require_toolchain warn
exit 0
