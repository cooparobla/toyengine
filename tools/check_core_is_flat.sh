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
# docs/ and .claude/skills/ are roots too, and they are the ones this gate was extended to cover
# after the split left them behind. The agent-facing docs are the engine's instructions to its
# next author, so a stale one is worse than a stale comment: the create-terrain skill still told
# an author to write `planet: true` and `planet_coarse_builds`, keys the engine had stopped
# parsing, and a YAML key the engine does not parse fails SILENTLY. The editor manual still
# documented an `Add > Planet` menu item that no longer exists. Neither was caught, because code
# was gated and prose was not.
#
# THE RULE IS PER LINE, AND IT IS NOT "never say planet": a doc may name a planet while pointing
# at the project that has them, and the word is how a reader searching for planets FINDS that
# pointer (src/toyengine/world/README.md's "see planetoy" section is the canonical one). So a
# line naming a planet passes when it also names `planetoy`, and fails when it does not --
# "planetoy does it" passes, "set planet: true" does not. That is deliberately sharper than the
# whole-file exemption for READMEs this replaced, which let a README document a planet key as
# though it were the engine's and still pass.
#
# `globe` is deliberately NOT matched. It is an ordinary English word with legitimate unrelated
# uses here -- a lamp's glass globe in an object-asset test, and the standard turntable-vs-
# trackball distinction for a model viewer -- and renaming those to satisfy a grep would be the
# tail wagging the dog. `planet` and `cube-sphere` carry no such ambiguity.
#
# assets/ is NOT a root, for one specific reason: the sky and cloud shaders are a physical
# atmosphere model (sky_atmosphere.glsl and friends), in which "the planet" is the ground sphere
# the air sits on, and a FLAT world is rendered by pinning a fake one under the camera. That is
# the flat path describing itself, not planet support, and the word is the model's own. The
# shader-side seam a round world does use is TERRAIN_CURVED (assets/shaders/terrain_styled.glsl).
#
# Usage: tools/check_core_is_flat.sh   (exit 0 clean, 1 with findings listed)
set -uo pipefail

cd "$(dirname "$0")/.."

readonly ROOTS=(src/toyengine src/editor src/tests docs .claude/skills)
readonly PATTERN='planet|cube.?sphere'

# A line that names planetoy is pointing at the project that has planets, which is allowed and
# is how the pointers stay findable. Everything else naming a planet is a finding.
hits=$(grep -rniE "$PATTERN" "${ROOTS[@]}" 2>/dev/null | grep -viE 'planetoy') || true

if [[ -n "$hits" ]]; then
    echo "$hits"
    echo
    echo "toyengine must not know about planetary worlds -- $(wc -l <<<"$hits") reference(s) above."
    echo "Planetary code belongs in a project (see planetoy); the engine offers seams, not shapes."
    echo "A doc may name a planet only while naming planetoy, so a reader can find the pointer."
    exit 1
fi

echo "core is flat: no planetary references in ${ROOTS[*]}"
