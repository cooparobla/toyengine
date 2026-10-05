#!/usr/bin/env bash
# package.sh [dev|ship] [out_dir] -- Build > Build from the command line: compile the game, stage
# its assets and bundle a relocatable, signed package for this platform (a .app on macOS, a
# folder on Linux; Shipping also archives it, and notarizes on macOS when build_settings.yaml
# names a notary profile). Default: dev into build/dist/development.
#   package.sh assets [out_dir]  -- only the encoded assets/ folder (Build > Package Assets).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"
mode="${1:-dev}"
bin="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
[[ -x "$bin" ]] || die "$bin not built -- run ./build.sh"
case "$mode" in
    dev|development|ship|shipping)
        if [[ -n "${2:-}" ]]; then exec "$bin" "$PROJECT_DIR" --build "$mode" --out "$2"; fi
        exec "$bin" "$PROJECT_DIR" --build "$mode" ;;
    assets) exec "$bin" "$PROJECT_DIR" --package "${2:-$PROJECT_DIR/dist}" ;;
    *) die "usage: package.sh [dev|ship|assets] [out_dir]" ;;
esac
