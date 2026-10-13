#!/usr/bin/env bash
# package.sh [dev|ship|assets] [out_dir]
#
# Build > Build from the command line: compiles the game, stages its assets and bundles a
# relocatable, signed package for the platform it runs on -- a .app on macOS, a folder with the
# game's libraries in lib/ on Linux. Shipping also archives it, and notarizes on macOS when
# build_settings.yaml names a notary profile. Default: dev into build/dist/development.
#
#   dev     | development  developer build, fast to produce
#   ship    | shipping     players' build (no debug hooks), archived
#   assets  [out_dir]      only the encoded assets/ folder (Build > Package Assets)
#
# Runs through this project's editor binary, and builds it first if it is not built yet.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

case "${1:-}" in -h|--help) usage ;; esac
mode="${1:-dev}"
bin="${BUILD_DIR:-$PROJECT_DIR/build}/${TOY_TARGET}_editor"
[[ "$bin" = /* ]] || bin="$PROJECT_DIR/$bin"
ensure_assets
ensure_binary "$bin"
case "$mode" in
    dev|development|ship|shipping)
        if [[ -n "${2:-}" ]]; then exec "$bin" "$PROJECT_DIR" --build "$mode" --out "$2"; fi
        exec "$bin" "$PROJECT_DIR" --build "$mode" ;;
    assets) exec "$bin" "$PROJECT_DIR" --package "${2:-$PROJECT_DIR/dist}" ;;
    *) die "usage: package.sh [dev|ship|assets] [out_dir]" ;;
esac
