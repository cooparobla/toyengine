#!/bin/bash
# render_scene.sh -- render a scene HEADLESS and save one frame, for verifying new assets.
#
# Usage:
#   .claude/skills/create-scene/scripts/render_scene.sh <scene> [out.png] [key=value ...]
#
#   <scene>      a scene name under assets/scenes (e.g. fog_test) or a path to a scene.yaml
#   out.png      where to write the frame (default: $TMPDIR/toyengine_render/<scene>.png)
#   key=value    config overrides applied to a scratch copy of assets/config.yaml, matched
#                against the `  key:` line of the file, e.g.
#                  debug_view=volumetrics   fog_enabled=true   ssr_jitter=0.0
#                  save_low_res=true        (the internal-resolution buffer, no UI)
#
# Never opens a window (window.visible=false, HEADLESS=1), uses a fixed timestep and no input,
# so the same scene renders the same frame run to run. Runs 150 frames by default; set
# FRAMES=<n> to change it (temporal effects need ~60+ to converge).
#
# Prints the output path on success. The engine log goes next to the image as <name>.log --
# check it for "not found", "warning" and "error" lines.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$repo"

if [ $# -lt 1 ]; then
    sed -n '2,20p' "${BASH_SOURCE[0]}"
    exit 2
fi
scene="$1"; shift

out=""
if [ $# -gt 0 ] && [[ "$1" == *.png ]]; then out="$1"; shift; fi
scratch="${TMPDIR:-/tmp}/toyengine_render"
mkdir -p "$scratch"
base="$(basename "${scene%/scene.yaml}")"; base="${base%.yaml}"
[ -n "$out" ] || out="$scratch/$base.png"
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"

if [ ! -x build/toyengine ]; then
    echo "build/toyengine not found -- build first: cmake --build build -j10" >&2
    exit 1
fi

cfg="$scratch/config_$base.yaml"
# Every `  key:` line is replaced -- keys are unique across config.yaml's sections in practice.
args=(-e 's/^  visible: true /  visible: false /'
      -e "s|^  filepath: \"[^\"]*\"|  filepath: \"$out\"|")
for kv in "$@"; do
    key="${kv%%=*}"; val="${kv#*=}"
    args+=(-e "s|^  $key:[^#]*|  $key: $val |")
done
sed "${args[@]}" assets/config.yaml > "$cfg"
for kv in "$@"; do
    key="${kv%%=*}"
    grep -q "^  $key:" "$cfg" || echo "warning: '$key' is not a top-level key in assets/config.yaml (it may be commented out) -- override ignored" >&2
done

rm -f "$out"
log="${out%.png}.log"
if ! HEADLESS=1 CONFIG="$cfg" SCENE="$scene" MAX_FRAMES="${FRAMES:-150}" FIXED_DT=0.016 NO_INPUT=1 \
        ./build/toyengine > "$log" 2>&1; then
    echo "toyengine exited with an error -- see $log" >&2
    tail -20 "$log" >&2
    exit 1
fi
if [ ! -f "$out" ]; then
    echo "no frame was written -- see $log" >&2
    exit 1
fi
grep -iE "not found|error|warning" "$log" | grep -v "^\s*$" | head -20 >&2 || true
echo "$out"
