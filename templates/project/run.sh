#!/usr/bin/env bash
# run.sh [scene] -- runs the game. A scene name expands to assets/scenes/<name>/scene.yaml.
#   HEADLESS=1 MAX_FRAMES=600 ./run.sh   (no window), BUILD_DIR=build-debug ./run.sh, ...
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"
bin="${BUILD_DIR:-$PROJECT_DIR/build}/$TOY_TARGET"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
[[ -x "$bin" ]] || die "$bin not built -- run ./build.sh"
cd "$PROJECT_DIR"
exec "$bin" "$@"
