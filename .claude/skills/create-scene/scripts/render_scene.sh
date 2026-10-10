#!/bin/bash
# render_scene.sh -- render a scene HEADLESS and save one frame, for verifying new assets.
#
# Usage:
#   .claude/skills/create-scene/scripts/render_scene.sh <scene> [out.png] [key=value ...]
#
#   <scene>      a scene name under assets/scenes (e.g. fog_demo) or a path to a scene.yaml
#   out.png      where to write the frame (default: $TMPDIR/toyengine_render/<scene>.png)
#   key=value    config overrides applied to a scratch copy of assets/config.yaml, matched
#                by key name in whatever layout the file is in (block, or the editor's flow
#                style); keys are unique across its sections in practice, e.g.
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
# A bare name is found anywhere under assets/scenes (demos live in area folders, e.g.
# effects/weather_demo), then passed on as a path.
if [[ "$scene" != *.yaml && "$scene" != *.caml ]]; then
    found="$(find assets/scenes -type d -name "$scene" -exec test -f {}/scene.yaml \; -print 2>/dev/null | head -1)"
    if [ -z "$found" ]; then echo "no scene named '$scene' under assets/scenes" >&2; exit 1; fi
    scene="$found/scene.yaml"
fi

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
# Overrides by key name, in either layout the file may be in: block (`  key: value  # note`) or
# the editor's flow style (`section: { key: value, ... }`). A value runs to the next `,` `}` `#`
# or end of line, or is a whole [list]. Window hidden and the output path always set.
python3 - "$cfg" "$out" "$@" <<'PY'
import re, sys
cfg, out, overrides = sys.argv[1], sys.argv[2], sys.argv[3:]
text = open("assets/config.yaml").read()
def put(text, key, val):
    pat = re.compile(r'(?<![\w])(' + re.escape(key) + r':[ \t]*)(\[[^\]]*\]|"[^"\n]*"|[^,}#\n]*?)(?=[ \t]*(?:[,}#\n]|$))')
    new, n = pat.subn(lambda m: m.group(1) + val, text, count=1)
    return new, n > 0
text, _ = put(text, "visible", "false")
text, _ = put(text, "filepath", '"' + out + '"')
for kv in overrides:
    key, _, val = kv.partition("=")
    text, ok = put(text, key, val)
    if not ok:
        print("warning: '%s' is not a key in assets/config.yaml -- override ignored" % key, file=sys.stderr)
open(cfg, "w").write(text)
PY

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
