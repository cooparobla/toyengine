#!/usr/bin/env bash
# clean.sh [--all] -- removes build output; --all also removes .libs/ (setup.sh re-fetches it).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"
rm -rf "$PROJECT_DIR"/build "$PROJECT_DIR"/build-*
if [[ "${1:-}" == "--all" ]]; then
    # A linked engine is only a symlink: remove the link, never the checkout behind it.
    if [[ -L "$ENGINE_DIR" ]]; then rm "$ENGINE_DIR"; fi
    rm -rf "$PROJECT_DIR/.libs"
fi
echo "[clean] done"
