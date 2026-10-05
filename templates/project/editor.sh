#!/usr/bin/env bash
# editor.sh [--scene <scene.yaml>] -- opens this project in its editor (built with src/ linked
# in, so your components load and Play runs them). The first open creates assets/.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"
bin="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
[[ -x "$bin" ]] || die "$bin not built -- run ./build.sh"
cd "$PROJECT_DIR"
exec "$bin" "$PROJECT_DIR" "$@"
