#version 450

// `editor_paint` surface shader, fragment stage: shows the colour editor_paint.vert's preview
// mesh carries (see that file: rg in the UV, blue as the bitangent's length - 1) as a matte
// albedo -- the paint, not the material, is what is being looked at.

#define GFX_SURFACE_FRAGMENT
#define GFX_SURFACE_NO_SNOW   // the paint, not the weather, is what is being looked at
#include <gfx/surface/gbuffer_fs.glsl>

void gfx_surface_fragment(inout GfxSurface s) {
    s.albedo    = clamp(vec3(s.uv, length(s.tbn[1]) - 1.0), 0.0, 1.0);
    s.metallic  = 0.0;
    s.roughness = 0.9;
    s.emissive  = vec3(0.0);
    s.normal_ws = normalize(s.tbn[2]);   // no normal map: the painted surface as modelled
}
