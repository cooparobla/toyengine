#!/usr/bin/env bash
# Capture the editor manual's screenshots headless (no window ever opens) and store them as
# JPEGs in docs/images/editor/.
#
# Usage: .claude/skills/editor-user-docs/scripts/capture_screenshots.sh [shot-filter...]
#   No filter: every shot (test) in toyengine_editor_docs' "docs" suite
#   (tests/editor/docs/docs_test.cpp). A filter is a substring of a shot name, e.g.
#   `capture_screenshots.sh sculpt timeline`.
#
# Each test runs on its own and is retried up to 3 times: the editor's headless UI tests hit a
# known, flaky MoltenVK segfault that has nothing to do with the shot.
set -u
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
BIN="$REPO/build/tests/toyengine_editor_docs"
OUT="$REPO/docs/images/editor"
RAW="${TMPDIR:-/tmp}/toyengine_docs_shots"

cmake --build "$REPO/build" -j --target toyengine_editor_docs >/dev/null || { echo "build failed"; exit 1; }
mkdir -p "$OUT" "$RAW"

tests=$("$BIN" --suite docs --list | awk -F'\t' '$1 == "docs" { print $2 }')
if [ $# -gt 0 ]; then
    tests=$(for t in $tests; do for f in "$@"; do [[ "$t" == *"$f"* ]] && echo "$t"; done; done | sort -u)
fi
[ -z "$tests" ] && { echo "no docs tests match"; exit 1; }

failed=()
for t in $tests; do
    ok=0
    for attempt in 1 2 3; do
        # "docs/<shot>" matches that shot's suite/name prefix (the runner filters by substring).
        if (cd "$REPO/build" && DOCS_SHOT_DIR="$RAW" "$BIN" --suite docs "docs/$t" >"$RAW/$t.log" 2>&1); then
            ok=1; break
        fi
        echo "$t: attempt $attempt failed (exit $?), retrying"
    done
    [ $ok = 1 ] && echo "$t: ok" || failed+=("$t")
done

# PNG -> JPEG (quality 85) so the repo stays small; sips ships with macOS.
for png in "$RAW"/*.png; do
    [ -e "$png" ] || continue
    sips -s format jpeg -s formatOptions 85 "$png" --out "$OUT/$(basename "${png%.png}").jpg" >/dev/null
done

echo "screenshots in ${OUT#$REPO/}/"
[ ${#failed[@]} -eq 0 ] || { echo "failed after 3 attempts: ${failed[*]} (logs in $RAW)"; exit 1; }
