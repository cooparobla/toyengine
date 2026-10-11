// heightmap_terrain_surface.glsl -- the `heightmap_terrain` surface shader's hooks
// (toyengine/world/heightmap_terrain.h). gfx_params.x is the terrain's slot in the surface world's
// terrain table (gfx_world.terrain[]).
//
// Vertex (G-buffer, both shadows, their tessellated twins): the flat patch vertex is placed on the
// height grid (heightmap_terrain_common.glsl), then raised by deep snow where the terrain has it
// enabled and the ground is flat enough to hold it.
//
// Fragment: per-pixel normal from the grid; eight paint layers blended by the two splat maps, each
// sampled top-down or -- for a layer marked triplanar, on steep ground -- from three axes, with its
// normal map blended onto the grid normal; then rain (wetness, puddles in hollows) and deep snow.
// The backbone's snow cover runs after this, unless the material opts out (deep-snow terrains do:
// deep snow IS their snow).

#include "heightmap_terrain_common.glsl"
#define SNOW_SURFACE_NO_HOOKS
#include "snow_surface.glsl"

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    int slot = ht_slot(gfx_params.x);
    vec2 t = ht_grid_coord(slot, v.position_os, v.model);
    vec3 p = ht_world(slot, t);
    vec3 n = ht_normal(slot, t);
    if (ht_deep_snow(slot)) p.z += gfx_snow_height(p) * ht_snow_hold(n);
    v.position_ws = p;
    v.normal_ws = n;
    v.tangent_ws = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    v.uv = t / vec2(gfx_world.terrain[slot].dims.xy - 1);
}
#endif // GFX_SURFACE_VERTEX

#ifdef GFX_SURFACE_FRAGMENT
#include <gfx/surface/wetness.glsl>

layout(set = 3, binding = 0) uniform sampler2D u_splat0;
layout(set = 3, binding = 1) uniform sampler2D u_splat1;
layout(set = 3, binding = 2) uniform sampler2DArray u_layer_albedo;
layout(set = 3, binding = 3) uniform sampler2DArray u_layer_normal;

/// A layer's albedo and tangent-space normal xy (scaled by its strength) at this point.
/// Planar layers sample top-down; triplanar ones blend three projections on slopes.
void ht_sample_layer(int slot, int i, vec3 p, vec3 n, out vec3 albedo, out vec3 nrm_ws) {
    vec4 L = gfx_world.terrain[slot].layer[i];
    vec4 T = gfx_world.terrain[slot].layer_tri[i];
    float inv_tiling = L.x;
    bool has_normal = T.z > 0.5;
    float strength = L.w;

    if (T.x > 0.5 && n.z < 0.95) {
        // Triplanar: blend weights from the normal, sharpened.
        vec3 w = pow(abs(n), vec3(max(T.y, 1.0)));
        w /= max(w.x + w.y + w.z, 1e-5);
        vec2 uv_x = p.yz * inv_tiling;
        vec2 uv_y = p.xz * inv_tiling;
        vec2 uv_z = p.xy * inv_tiling;
        albedo = texture(u_layer_albedo, vec3(uv_x, i)).rgb * w.x +
                 texture(u_layer_albedo, vec3(uv_y, i)).rgb * w.y +
                 texture(u_layer_albedo, vec3(uv_z, i)).rgb * w.z;
        if (has_normal) {
            // Whiteout blend per projection, swizzled back to world space.
            vec2 tx = (texture(u_layer_normal, vec3(uv_x, i)).xy * 2.0 - 1.0) * strength;
            vec2 ty = (texture(u_layer_normal, vec3(uv_y, i)).xy * 2.0 - 1.0) * strength;
            vec2 tz = (texture(u_layer_normal, vec3(uv_z, i)).xy * 2.0 - 1.0) * strength;
            vec3 nx = vec3(0.0, tx.x, tx.y) * sign(n.x);
            vec3 ny = vec3(ty.x, 0.0, ty.y) * sign(n.y);
            vec3 nz = vec3(tz.x, tz.y, 0.0);
            nrm_ws = nx * w.x + ny * w.y + nz * w.z;
        } else {
            nrm_ws = vec3(0.0);
        }
        return;
    }

    vec2 uv = p.xy * inv_tiling;
    albedo = texture(u_layer_albedo, vec3(uv, i)).rgb;
    nrm_ws = has_normal ? vec3((texture(u_layer_normal, vec3(uv, i)).xy * 2.0 - 1.0) * strength, 0.0) : vec3(0.0);
}

void gfx_surface_fragment(inout GfxSurface s) {
    int slot = ht_slot(gfx_params.x);
    vec3 p = s.position_ws;
    vec2 t = ht_grid_coord_ws(slot, p);
    vec3 n = ht_normal(slot, t);

    vec2 suv = clamp((p.xy - gfx_world.terrain[slot].origin.xy) / gfx_world.terrain[slot].cell.zw, 0.0, 1.0);
    vec4 w0 = texture(u_splat0, suv);
    vec4 w1 = texture(u_splat1, suv);
    float w[8] = float[](w0.r, w0.g, w0.b, w0.a, w1.r, w1.g, w1.b, w1.a);
    int count = clamp(gfx_world.terrain[slot].dims.z, 1, 8);
    float total = 0.0;
    for (int i = 0; i < count; ++i) total += w[i];
    if (total < 1e-4) { w[0] = 1.0; total = 1.0; }

    vec3 albedo = vec3(0.0);
    vec3 bend = vec3(0.0);
    float roughness = 0.0, metallic = 0.0;
    for (int i = 0; i < count; ++i) {
        float wi = w[i] / total;
        if (wi < 0.004) continue;
        vec3 a, b;
        ht_sample_layer(slot, i, p, n, a, b);
        albedo    += wi * a * gfx_world.terrain[slot].tint[i].rgb;
        bend      += wi * b;
        roughness += wi * gfx_world.terrain[slot].layer[i].y;
        metallic  += wi * gfx_world.terrain[slot].layer[i].z;
    }
    // The layers' normal maps tilt the grid normal (whiteout-style: their world-space xy offsets
    // added on, top-down layers in the xy plane, triplanar ones per projection axis).
    vec3 nb = normalize(n + bend);

    s.albedo    = albedo;
    s.roughness = max(roughness, 0.045);
    s.metallic  = metallic;
    s.normal_ws = nb;

    // Rain: wet darkening everywhere open to the sky, and puddles where water gathers.
    float puddle = 0.0;
    if (ht_puddles(slot)) {
        float h = ht_height(slot, t);
        float around = 0.25 * (ht_height(slot, t + vec2(3.0, 0.0)) + ht_height(slot, t - vec2(3.0, 0.0)) +
                               ht_height(slot, t + vec2(0.0, 3.0)) + ht_height(slot, t - vec2(0.0, 3.0)));
        float cavity = around - h;   // > 0 in hollows (m)
        puddle = smoothstep(0.02, 0.25, cavity) * smoothstep(0.96, 0.995, n.z);
    }
    gfx_wet_apply(s, n, puddle);

    if (ht_deep_snow(slot)) {
        // Deep snow only where the ground holds it -- its own normal / look over the layers.
        float hold = ht_snow_hold(n);
        if (hold > 0.0) {
            GfxSurface ground = s;
            gfx_deep_snow_fragment(s);
            s.albedo    = mix(ground.albedo, s.albedo, hold);
            s.roughness = mix(ground.roughness, s.roughness, hold);
            s.metallic  = mix(ground.metallic, s.metallic, hold);
            s.normal_ws = normalize(mix(ground.normal_ws, s.normal_ws, hold));
        }
    }
}
#endif // GFX_SURFACE_FRAGMENT
