#ifndef GFX_SDF_SCENE_BODY_GLSL
#define GFX_SDF_SCENE_BODY_GLSL

// gfx/sdf_scene_body.glsl -- per-renderer SDF evaluation, raymarch, and
// normal estimation, built on gfx/sdf.glsl's primitive/operator math.
//
// A "body" file in the same sense as gfx/ssr_trace_body.glsl: it is NOT
// self-contained. The includer must, BEFORE including this file:
//   1. #include <gfx/sdf.glsl>                      -- for the SdfShapeGpu/
//      SdfRendererGpu struct types this file's buffer declarations below use.
//   2. Declare a readonly buffer exposing an `SdfRendererGpu sdf_renderers[]`
//      array, and another exposing an `SdfShapeGpu sdf_shapes[]` array, at
//      whatever set/binding indices that shader wants (see
//      gfxcoopa/engine/data/sdf_data.h's SdfData for the buffers' CPU-side
//      layout and lifetime).
// This split -- rather than this file declaring the buffers itself -- is
// what lets the five SDF passes (G-buffer/forward/shadow x2/capture) share
// one evaluation body while binding it at a different set index each, the
// same reason ssr_trace_body.glsl doesn't declare its own G-buffer/Hi-Z
// sampler bindings either.

#include <gfx/sdf.glsl>

/// Evaluates one SdfRenderer's combined signed distance field at world point
/// `p` -- folds every SdfShapeGpu in `sdf_renderers[renderer_index]`'s
/// [first, first+count) range via its own op/blend, in array order (the same
/// order SdfRenderer::collect_shapes() walked the scene hierarchy in). The
/// first shape in the range always UNIONs in regardless of its own `op`
/// field: there is nothing yet to subtract from or intersect against.
float gfx_sdf_scene_eval(uint renderer_index, vec3 p) {
    SdfRendererGpu r = sdf_renderers[renderer_index];
    uint first = r.range.x;
    uint count = r.range.y;

    float d = 1e30;
    for (uint i = 0u; i < count; ++i) {
        SdfShapeGpu s = sdf_shapes[first + i];
        vec3 local_p = (s.inv_world * vec4(p, 1.0)).xyz;
        float scale = max(s.type_op_blend.w, 1e-4);
        float sd = gfx_sdf_prim_dist(local_p, s.type_op_blend.x, s.params_round.xyz, s.params_round.w) * scale;

        if (i == 0u) {
            d = sd;
            continue;
        }

        int op = int(s.type_op_blend.y + 0.5);
        float k = s.type_op_blend.z;
        if (op == GFX_SDF_OP_SUBTRACT) {
            d = gfx_sdf_smooth_subtract(d, sd, k);
        } else if (op == GFX_SDF_OP_INTERSECT) {
            d = gfx_sdf_smooth_intersect(d, sd, k);
        } else {
            d = gfx_sdf_smooth_union(d, sd, k);
        }
    }
    return d;
}

/// One sphere-trace's result.
struct GfxSdfHit {
    vec3  pos;   ///< World-space position -- the hit point if `hit`, else the march's last sample.
    float t;     ///< Ray parameter at `pos`.
    int   steps; ///< Iterations actually taken (for step-count debug visualization).
    bool  hit;   ///< True if the field dropped below the surface epsilon before `t_end`/`max_steps`.
};

/// Sphere-traces renderer `renderer_index`'s field along [ro + rd*t_start,
/// ro + rd*t_end] (the caller clips this range to the object's world AABB
/// via gfx_sdf_aabb_intersect() first, so a ray that misses the object
/// entirely costs one slab test, never a march). Step size is the field
/// value itself (standard sphere tracing), floored at half the surface
/// epsilon so a near-degenerate field can't stall the loop.
GfxSdfHit gfx_sdf_march_renderer(uint renderer_index, vec3 ro, vec3 rd,
                                 float t_start, float t_end, int max_steps, float epsilon) {
    GfxSdfHit result;
    result.pos   = ro + rd * t_start;
    result.t     = t_start;
    result.steps = 0;
    result.hit   = false;

    if (t_end <= t_start) {
        return result;
    }

    float t = t_start;
    float min_step = max(epsilon * 0.5, 1e-5);
    for (int i = 0; i < max_steps; ++i) {
        result.steps = i + 1;
        vec3 p = ro + rd * t;
        float d = gfx_sdf_scene_eval(renderer_index, p);
        if (d < epsilon) {
            result.pos = p;
            result.t   = t;
            result.hit = true;
            return result;
        }
        t += max(d, min_step);
        if (t > t_end) {
            break;
        }
    }
    result.t   = t;
    result.pos = ro + rd * t;
    return result;
}

/// Tetrahedral central-difference normal estimator (Inigo Quilez) -- 4 field
/// evaluations instead of the naive 6-tap central difference.
vec3 gfx_sdf_normal_renderer(uint renderer_index, vec3 p, float h) {
    const vec2 k = vec2(1.0, -1.0);
    return normalize(
        k.xyy * gfx_sdf_scene_eval(renderer_index, p + k.xyy * h) +
        k.yyx * gfx_sdf_scene_eval(renderer_index, p + k.yyx * h) +
        k.yxy * gfx_sdf_scene_eval(renderer_index, p + k.yxy * h) +
        k.xxx * gfx_sdf_scene_eval(renderer_index, p + k.xxx * h));
}

#endif // GFX_SDF_SCENE_BODY_GLSL
