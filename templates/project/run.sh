#!/usr/bin/env bash
# run.sh [scene] [--headless] [--frames N] [--debug] [-- <game args>]
#
# Plays the game. A bare scene name expands to assets/scenes/<name>/scene.yaml (the engine's
# own assets/scenes are found too); with no scene it loads config.yaml's scene.default_scene.
# Builds the game first if it is not built yet, and creates assets/ if the project has none.
#
#   --headless     no window, vsync off (stops after --frames, default 600)
#   --frames N     exit after N frames
#   --debug        run the build-debug/ binary (Vulkan validation on)
#
# The env vars work on macOS and Linux alike: HEADLESS=1 MAX_FRAMES=600 ./run.sh,
# SCENE=assets/scenes/x/scene.yaml ./run.sh, CONFIG=my_config.yaml ./run.sh,
# BUILD_DIR=build-ship ./run.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

args=()
while (( $# )); do
    case "$1" in
        -h|--help)   usage ;;
        --headless)  export HEADLESS=1; shift ;;
        --frames)    [[ -n "${2:-}" ]] || die "--frames needs a number"; export MAX_FRAMES="$2"; shift 2 ;;
        --debug)     BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-debug}"; shift ;;
        --)          shift; args+=("$@"); break ;;
        *)           args+=("$1"); shift ;;
    esac
done
# Headless has no window to close: without a frame limit the run would never end.
if [[ "${HEADLESS:-0}" != 0 && -z "${MAX_FRAMES:-}" ]]; then export MAX_FRAMES=600; fi

bin="${BUILD_DIR:-$PROJECT_DIR/build}/$TOY_TARGET"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
ensure_assets
ensure_binary "$bin"
cd "$PROJECT_DIR"
exec "$bin" ${args[@]+"${args[@]}"}
