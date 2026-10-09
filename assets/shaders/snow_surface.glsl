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
    const float e = 0.05;   // finite-difference step for the cheap terms (5 cm)
    float h, lying, edge = 1.0;
    vec2 grad;
    if (gfx_world.snow_style.x > 0.5) {
        // Hard patches: h = A * M - T, with A = depth * cover * open (cheap), M the patch mask
        // (the expensive pattern) and T the trench (cheap). The pattern is evaluated ONCE, with
        // its analytic gradient, for the mask, the normal and the crisp colour edge -- not once
        // per finite-difference tap -- and not at all where the mask is saturated (full cover).
        vec2 dpdx = dFdx(base.xy), dpdy = dFdy(base.xy);
        float cover = gfx_world.snow.x, depth = gfx_world.snow.y;
        vec3 lift = vec3(0.0, 0.0, 0.35 + 0.35 * gfx_world.occl.z);
        float open = gfx_world_open_sky(base + lift, 0.5);
        float A = depth * cover * open;
        vec2 dA = depth * cover / (2.0 * e) *
                  vec2(gfx_world_open_sky(base + lift + vec3(e, 0.0, 0.0), 0.5) - gfx_world_open_sky(base + lift - vec3(e, 0.0, 0.0), 0.5),
                       gfx_world_open_sky(base + lift + vec3(0.0, e, 0.0), 0.5) - gfx_world_open_sky(base + lift - vec3(0.0, e, 0.0), 0.5));
        float M = 1.0;
        vec2 dM = vec2(0.0);
        if (!gfx_snow_patch_saturated(cover, open)) {
            vec3 field = gfx_snow_blobs_grad(base.xy, gfx_world.snow_style.y);
            float v = gfx_snow_patch_value_from_field(field.x, cover, open);
            M = gfx_snow_patch_ramp(v);
            dM = gfx_snow_patch_ramp_slope(v) * field.yz;
            // The colour edge is crisp on top of the ramped mound wall.
            edge = gfx_snow_patch_edge(v, field.yz, dpdx, dpdy);
        }
        float T = gfx_world_trench(base.xy);
        vec2 dT = vec2(gfx_world_trench(base.xy + vec2(e, 0.0)) - gfx_world_trench(base.xy - vec2(e, 0.0)),
                       gfx_world_trench(base.xy + vec2(0.0, e)) - gfx_world_trench(base.xy - vec2(0.0, e))) / (2.0 * e);
        lying = A * M;
        h = lying > 0.0 ? max(0.0, lying - T) : 0.0;
        grad = h > 0.0 ? dA * M + A * dM - dT : vec2(0.0);
    } else {
        h = gfx_snow_height(base);
        // Normal from the height gradient (central differences, 5 cm apart).
        grad = vec2(gfx_snow_height(base + vec3(e, 0.0, 0.0)) - gfx_snow_height(base - vec3(e, 0.0, 0.0)),
                    gfx_snow_height(base + vec3(0.0, e, 0.0)) - gfx_snow_height(base - vec3(0.0, e, 0.0))) / (2.0 * e);
        lying = gfx_snow_lying_height(base);
    }
    vec3 n_snow = normalize(vec3(-grad, 1.0));

    // How much the snow hides the material: full past a few centimetres; the pressed-down floor
    // of a trench is compacted, greyer snow, and bare where pressed to the ground.
    float amount = smoothstep(0.0, 0.03, h) + (1.0 - smoothstep(0.0, 0.03, h)) * smoothstep(0.0, 0.02, lying) * 0.6;
    amount *= edge;
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
