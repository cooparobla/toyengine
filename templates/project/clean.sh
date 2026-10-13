#!/usr/bin/env bash
# clean.sh [--all]
#
# Removes this project's build output (build/, build-debug/, build-ship/, dist/).
#
#   --all   also removes .libs/, the engine checkout -- setup.sh re-fetches it, and the next
#           build is a full one again. A linked engine is only a symlink: the link goes, the
#           checkout behind it never does.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/.toyenv.sh"

all=0
case "${1:-}" in
    -h|--help) usage ;;
    --all)     all=1 ;;
    "")        ;;
    *)         die "unknown argument '$1' (see ./clean.sh --help)" ;;
esac

rm -rf "$PROJECT_DIR"/build "$PROJECT_DIR"/build-* "$PROJECT_DIR"/dist
if (( all )); then
    if [[ -L "$ENGINE_DIR" ]]; then rm "$ENGINE_DIR"; fi
    rm -rf "$PROJECT_DIR/.libs"
fi
echo "[clean] done"
