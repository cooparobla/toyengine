#!/usr/bin/env bash
# package.sh [out_dir=dist] -- assets/ (YAML encoded to .caml) + the engine's runtime files +
# the game binary, into out_dir. Same as the editor's Build > Package.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"
out="${1:-$PROJECT_DIR/dist}"
bin="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
[[ -x "$bin" ]] || die "$bin not built -- run ./build.sh"
exec "$bin" "$PROJECT_DIR" --package "$out"
