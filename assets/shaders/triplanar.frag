#version 450

// `triplanar` surface shader, G-buffer fragment stage: every material map (albedo, alpha mask,
// metallic-roughness, normal) is projected along the three axes and blended by the surface
// normal, instead of read through the mesh's UVs. Stretched, scaled or UV-less meshes keep an
// even texel density, and neighbouring objects in world space line up seamlessly.
//
// gfx_params (see PBRMaterial::shader_params):
//   x = tiling    -- texture repeats per unit of projected space (<= 0 means 1).
//   y = sharpness -- blend-weight exponent; higher narrows the seams between the three
//                    projections (<= 0 means 4).
//   z = space     -- 0 projects in WORLD space (the texture stays put while the object moves
//                    through it); 1 projects in OBJECT space (the texture rides along with the
//                    object's position, rotation and scale).
//   w = normal strength -- scales the normal map's tangent-space tilt (<= 0 means 1).
//
// Projections (Z-up): X-facing faces map (y, -z), Y-facing (-x, -z), Z-facing (x, y), each
// u mirrored by the normal's sign so no face reads mirror-imaged; side images stay upright.
//
// Material samplers clamp to edge, so tiling wraps here: texel-AA (as the stock gbuffer.frag
// applies through gfx/texel_aa.glsl) runs on the UNWRAPPED texel coordinate, whose screen
// derivatives stay continuous across tile seams, and only the integer texel index wraps.
//
// Shadow passes use the stock entry points -- an OPAQUE caster never samples a map there, so
// triplanar is meant for OPAQUE materials; a CUTOUT one would cast its mesh-UV silhouette.

#include <gfx/texel_aa.glsl>

layout(location = 6) in vec3 frag_object_pos;
layout(location = 7) in vec3 frag_object_normal;
layout(location = 8) flat in mat3 frag_normal_matrix;

vec4 triplanar_sample(sampler2D tex);

#define GFX_SURFACE_SAMPLE(tex, uv) triplanar_sample(tex)
// The backbone's own normal fetch reads a flat texel, so its TBN normal is the geometric one;
// gfx_surface_fragment() below replaces it with the blended triplanar normal.
#define GFX_SURFACE_SAMPLE_NORMAL(tex, uv) vec4(0.5, 0.5, 1.0, 1.0)
#define GFX_SURFACE_FRAGMENT
#include <gfx/surface/gbuffer_fs.glsl>

bool triplanar_local() { return gfx_params.z > 0.5; }

/// Blend weights and per-axis UVs (in texel-agnostic texture space) for this fragment.
struct Triplanar {
    vec3 w;    // x/y/z projection weights, summing to 1
    vec3 s;    // sign of the projection-space normal per axis (+-1, never 0)
    vec2 uv_x;
    vec2 uv_y;
    vec2 uv_z;
};

Triplanar triplanar_setup() {
    vec3 p = triplanar_local() ? frag_object_pos : frag_world_pos;
    vec3 n = normalize(triplanar_local() ? frag_object_normal : frag_world_normal);
    float tiling = gfx_params.x > 0.0 ? gfx_params.x : 1.0;
    float sharp  = gfx_params.y > 0.0 ? gfx_params.y : 4.0;

    Triplanar t;
    t.w = pow(abs(n), vec3(sharp));
    t.w /= max(t.w.x + t.w.y + t.w.z, 1e-6);
    t.s = vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);

    p *= tiling;
    t.uv_x = vec2( p.y * t.s.x, -p.z);
    t.uv_y = vec2(-p.x * t.s.y, -p.z);
    t.uv_z = vec2( p.x,          p.y * t.s.z);
    return t;
}

/// One wrapped, texel-AA'd fetch at an unwrapped (tiling) UV.
vec4 triplanar_fetch(sampler2D tex, vec2 uv) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 t = uv * size - 0.5;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec2 w = clamp(fwidth(t), 1e-6, 1.0);
    f = clamp((f - 0.5) / w + 0.5, 0.0, 1.0);
    i = mod(i, size);
    return textureLod(tex, (i + 0.5 + f) / size, 0.0);
}

vec4 triplanar_sample(sampler2D tex) {
    Triplanar t = triplanar_setup();
    return triplanar_fetch(tex, t.uv_x) * t.w.x
         + triplanar_fetch(tex, t.uv_y) * t.w.y
         + triplanar_fetch(tex, t.uv_z) * t.w.z;
}

void gfx_surface_fragment(inout GfxSurface s) {
    Triplanar t = triplanar_setup();
    float strength = gfx_params.w > 0.0 ? gfx_params.w : 1.0;

    // Tangent-space normals (green-up) per projection, tilt scaled by strength.
    vec3 tx = triplanar_fetch(u_normal_map, t.uv_x).xyz * 2.0 - 1.0;
    vec3 ty = triplanar_fetch(u_normal_map, t.uv_y).xyz * 2.0 - 1.0;
    vec3 tz = triplanar_fetch(u_normal_map, t.uv_z).xyz * 2.0 - 1.0;
    tx.xy *= strength;
    ty.xy *= strength;
    tz.xy *= strength;

    // Each into projection space as tangent * u-axis + bitangent * image-up + normal * axis:
    //   X: u = +-Y, up = +Z     Y: u = -+X, up = +Z     Z: u = +X, up = -+Y
    vec3 nx = vec3(tx.z * t.s.x,  tx.x * t.s.x, tx.y);
    vec3 ny = vec3(-ty.x * t.s.y, ty.z * t.s.y, ty.y);
    vec3 nz = vec3(tz.x,         -tz.y * t.s.z, tz.z * t.s.z);
    vec3 n = normalize(nx * t.w.x + ny * t.w.y + nz * t.w.z);

    if (triplanar_local()) n = normalize(frag_normal_matrix * n);
    if (!gl_FrontFacing) n = -n; // same double-sided correction the backbone applies
    s.normal_ws = n;
}
