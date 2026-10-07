#ifndef GFX_LOCAL_SHADOW_TYPES_GLSL
#define GFX_LOCAL_SHADOW_TYPES_GLSL

// gfx/local_shadow_types.glsl -- the std140 layout of the local-light (point/spot) shadow block,
// LocalShadowBlock in engine/data/light_data.h. Include BEFORE the uniform block that embeds it
// (toyengine's light_ubo_body.glsl, gfx/volumetrics_ubo.glsl); the sampling functions live in
// gfx/local_shadow.glsl.

#define GFX_MAX_LOCAL_SHADOWS      12
#define GFX_MAX_LOCAL_SHADOW_VIEWS 48

struct GfxLocalShadow {
    vec4 tile;    // xy = atlas uv of the light's first tile, z = tile edge in atlas uv,
                  // w = kind: 0 none, 1 spot, 2 point
    vec4 light;   // xyz = light position, w = far plane (range)
    vec4 proj;    // x = near plane, y = tan(half fov), z = tile edge in texels,
                  // w = first view index into view_proj
    vec4 pcf;     // x = spot penumbra width in world units, y = point penumbra radius in
                  // texels, z = largest radius in texels, w = darkness 0..1
};

struct GfxLocalShadowBlock {
    vec4 info;    // x = record count, y = atlas edge in texels, z = normal-offset bias in texels
    vec4 bias;    // x = constant depth bias (texels), y = slope depth bias (texels per tan),
                  // z = largest tan honoured
    GfxLocalShadow shadows[GFX_MAX_LOCAL_SHADOWS];
    mat4 view_proj[GFX_MAX_LOCAL_SHADOW_VIEWS];
};

#endif // GFX_LOCAL_SHADOW_TYPES_GLSL
