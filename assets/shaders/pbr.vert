#version 450

// Stock forward-transparent vertex shader -- identity displacement. Overrides gfxcoopa's
// base pbr.vert (which declares no push_constant block at all) with
// gfx/surface/transparent_vs.glsl's backbone, so a derived shader (e.g. water.vert) can
// override displacement via the same hook mechanism every other pass uses. Kept local to
// toyengine rather than editing gfxcoopa's base copy: PbrPipeline (gfxcoopa's own legacy,
// documented-unused forward path) also resolves "pbr.vert" and has its own,
// differently-shaped push-constant expectations -- leaving the base copy alone avoids any
// risk of disturbing it.
#include <gfx/surface/transparent_vs.glsl>
