#!/usr/bin/env bash
# Holds the line that toyengine is FLAT-TERRAIN ONLY and knows nothing about round worlds.
#
# Planetary support is its own project (planetoy), reaching the engine only through the seams
# documented in src/toyengine/core/module.h: a terrain parse hook, a world-bake hook, a
# world-frame source, the sky/cloud state hooks, ChunkShaper/TileGrid, and TerrainSystem's
# policy virtuals. Nothing under the roots below may name a planet -- not in code, and not in a
# comment either: a comment explaining a branch with "on a planet" is exactly the knowledge the
# engine should not have. Say "a world whose up is not +Z", or name the seam.
#
# `globe` is deliberately NOT matched. It is an ordinary English word with legitimate unrelated
# uses here -- a lamp's glass globe in an object-asset test, and the standard turntable-vs-
# trackball distinction for a model viewer -- and renaming those to satisfy a grep would be the
# tail wagging the dog. `planet` and `cube-sphere` carry no such ambiguity.
#
# Usage: tools/check_core_is_flat.sh   (exit 0 clean, 1 with findings listed)
set -uo pipefail

cd "$(dirname "$0")/.."

readonly ROOTS=(src/toyengine src/editor src/tests)
readonly PATTERN='planet|cube.?sphere'

# READMEs are excluded: they are allowed to say what the engine deliberately does NOT do, and
# to point a reader at the project that does it.
hits=$(grep -rniE "$PATTERN" "${ROOTS[@]}" 2>/dev/null | grep -v '/README\.md:') || true

if [[ -n "$hits" ]]; then
    echo "$hits"
    echo
    echo "toyengine must not know about planetary worlds -- $(wc -l <<<"$hits") reference(s) above."
    echo "Planetary code belongs in a project (see planetoy); the engine offers seams, not shapes."
    exit 1
fi

echo "core is flat: no planetary references in ${ROOTS[*]}"
