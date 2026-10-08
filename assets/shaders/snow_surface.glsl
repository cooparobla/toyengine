// snow_surface.glsl -- the `snow` surface shader: deep snow lying on a surface (a ground plane,
// a terrain-like mesh), raised by the weather's cover and carved by the trench field
// (SnowDeformer objects -- see toyengine/world/snow_system.h). Give its renderer tessellation
// (MeshRenderer `tessellation:`) so the trenches -- 10 cm cells -- have vertices to carve.
//
// Vertex (every backbone: G-buffer, both shadows, their tessellated twins): +Z by
// gfx_snow_height() of the vertex's undisplaced world position (gfx/surface/snow_height.glsl).
// Fragment: snow-white over the material where the snow is deeper than a few centimetres, the
// material showing through where it thins out or something pressed down to the ground; the
// normal comes from the height field's gradient, so trench walls shade as walls.
//
// The general cover layer (gfx/surface/snow.glsl) is turned off for this shader
// (GFX_SURFACE_NO_SNOW in snow.frag): this IS the snow.

#include <gfx/surface/snow_height.glsl>

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    v.position_ws.z += gfx_snow_height(v.position_ws);
}
#endif // GFX_SURFACE_VERTEX

#ifdef GFX_SURFACE_FRAGMENT
void gfx_surface_fragment(inout GfxSurface s) {
    if (gfx_world.snow.x <= 0.0 || gfx_world.snow.y <= 0.0) return;
    // The height field at this fragment's xy (the displacement is +Z only). Its z enters only
    // through the soft open-sky test, which a few centimetres either way does not move.
    vec3 base = s.position_ws;
    float h = gfx_snow_height(base);

    // Normal from the height gradient (central differences, 5 cm apart).
    const float e = 0.05;
    float hx = gfx_snow_height(base + vec3(e, 0.0, 0.0)) - gfx_snow_height(base - vec3(e, 0.0, 0.0));
    float hy = gfx_snow_height(base + vec3(0.0, e, 0.0)) - gfx_snow_height(base - vec3(0.0, e, 0.0));
    vec3 n_snow = normalize(vec3(-hx / (2.0 * e), -hy / (2.0 * e), 1.0));

    // How much the snow hides the material: full past a few centimetres; the pressed-down floor
    // of a trench is compacted, greyer snow, and bare where pressed to the ground.
    float lying = gfx_snow_lying_height(base);
    float amount = smoothstep(0.0, 0.03, h) + (1.0 - smoothstep(0.0, 0.03, h)) * smoothstep(0.0, 0.02, lying) * 0.6;
    float pressed = clamp(1.0 - h / max(lying, 1e-3), 0.0, 1.0) * step(1e-3, lying);
    vec3 snow_albedo = mix(GFX_SNOW_ALBEDO, GFX_SNOW_ALBEDO * vec3(0.78, 0.82, 0.88), pressed * 0.8);
    float sparkle = gfx_snow_hash_(floor(base.xy * 40.0)) * 0.06 * (1.0 - pressed);

    s.albedo    = mix(s.albedo, snow_albedo + sparkle, amount);
    s.roughness = mix(s.roughness, mix(0.85, 0.6, pressed), amount);
    s.metallic  = mix(s.metallic, 0.0, amount);
    s.emissive  = mix(s.emissive, vec3(0.0), amount);
    s.normal_ws = normalize(mix(s.normal_ws, n_snow, amount));
}
#endif // GFX_SURFACE_FRAGMENT
