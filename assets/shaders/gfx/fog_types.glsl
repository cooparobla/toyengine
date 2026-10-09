#ifndef GFX_FOG_TYPES_GLSL
#define GFX_FOG_TYPES_GLSL

// gfx/fog_types.glsl -- the global fog parameter block, a std140 mirror of the fog fields
// appended to gfxcoopa's LightUBO (light_data.h, fog_color .. fog_water). light_ubo_body.glsl
// embeds it as `lights.fog`, so every shader that shades from the light set (the opaque fog
// pass and each forward shader) reads one source. The math lives in gfx/fog.glsl.

struct GfxFogBlock {
    vec4 color;    // rgb = in-scatter colour, w = 1 enabled / 0 off
    vec4 density;  // x = mode (0 Linear, 1 Exponential), y = density (1/m at/below the base),
                   // z = linear start, w = linear end
    vec4 height;   // x = height base (world Z), y = falloff metres (<= 0 flat), z = sky blend,
                   // w = max opacity
    vec4 range;    // x = start distance, y = cutoff distance (0 = none), z = sky distance
    vec4 sun;      // rgb = sun colour * intensity * amount, w = HG anisotropy g
    vec4 sun_dir;  // xyz = direction sunlight travels, w = directional in-scatter start distance
    vec4 water;    // x = water level (world Z), y = 1 when the camera is under it
};

#endif // GFX_FOG_TYPES_GLSL
