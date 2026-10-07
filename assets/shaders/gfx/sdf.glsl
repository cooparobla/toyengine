#ifndef GFX_SDF_GLSL
#define GFX_SDF_GLSL

// gfx/sdf.glsl -- shared signed-distance-field primitive/operator math and
// GPU record types for the SDF raymarching system (see
// gfxcoopa/engine/data/sdf_data.h and gfxcoopa/engine/components/sdf_shape.h
// for the CPU-side counterparts these mirror byte-for-byte).
//
// Declares no uniforms, samplers, or storage blocks -- same rule as
// gfx/fog.glsl and gfx/ssr_common.glsl: every function takes its inputs as
// parameters, since different passes (G-buffer/forward/shadow/capture) bind
// their SdfGlobals UBO and renderer/shape SSBOs at different set indices.
// The two struct TYPES below are the one exception: a plain type definition
// declares no binding, so it's safe to share here -- the includer declares
// the actual `sdf_renderers`/`sdf_shapes` buffer INSTANCES (naming the
// binding/set it wants) using these types, then includes
// gfx/sdf_scene_body.glsl for the functions that read them (see that file's
// doc for why the split is necessary).

/// Shape type encoding -- matches SdfShapeType (sdf_shape.h) and
/// SdfGpuShapeType (sdf_data.h).
#define GFX_SDF_TYPE_SPHERE 0
#define GFX_SDF_TYPE_BOX    1
#define GFX_SDF_TYPE_PLANE  2

/// Boolean-op encoding -- matches SdfOperation (sdf_shape.h) and SdfGpuOp
/// (sdf_data.h).
#define GFX_SDF_OP_UNION     0
#define GFX_SDF_OP_SUBTRACT  1
#define GFX_SDF_OP_INTERSECT 2

/// std140 mirror of coopa::gfx::engine::data::SdfShapeGPU.
struct SdfShapeGpu {
    mat4 inv_world;
    vec4 params_round;   // xyz = SdfShape::params, w = rounding
    vec4 type_op_blend;  // x = type, y = op, z = blend k, w = uniform world scale
};

/// std140 mirror of coopa::gfx::engine::data::SdfRendererGPU.
struct SdfRendererGpu {
    vec4  clip_rect;      // xy = clip-space min, zw = clip-space max
    vec4  bounds_min;     // world AABB min (xyz)
    vec4  bounds_max;     // world AABB max (xyz)
    vec4  albedo_alpha;   // rgb = albedo, a = alpha
    vec4  mr_ao_cutoff;   // x = metallic, y = roughness, z = ao, w = alpha_cutoff
    vec4  emissive;       // xyz = pre-multiplied emissive radiance
    uvec4 range;          // x = first shape index, y = shape count, z = max_steps
    vec4  march;          // x = surface_epsilon, y = normal_epsilon
};

/// Exact signed distance to an axis-aligned box of half-extent `half_extent`,
/// centered at the local origin (Inigo Quilez's standard formula).
float gfx_sdf_box(vec3 p, vec3 half_extent) {
    vec3 q = abs(p) - half_extent;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

/// Dispatches to the primitive distance function named by `type`
/// (GFX_SDF_TYPE_*), in the shape's own local space (the caller has already
/// transformed the query point by the shape's inv_world).
///
/// `rounding` only affects Box: the box's OUTER half-extent stays `params`
/// (matching what an artist authored) while its corners round off with
/// radius `rounding`, via the standard shrink-then-grow rounded-box
/// technique. Sphere and Plane have no corners to round, so rounding is a
/// deliberate no-op for them (see SdfShape::rounding's doc).
float gfx_sdf_prim_dist(vec3 p, float type, vec3 params, float rounding) {
    int t = int(type + 0.5);
    if (t == GFX_SDF_TYPE_BOX) {
        float r = max(rounding, 0.0);
        vec3 half_extent = max(params - vec3(r), vec3(0.0001));
        return gfx_sdf_box(p, half_extent) - r;
    } else if (t == GFX_SDF_TYPE_PLANE) {
        return p.z - params.x;
    }
    return length(p) - params.x; // Sphere
}

/// Polynomial smooth union (Inigo Quilez). k <= 0 degenerates to a hard min().
float gfx_sdf_smooth_union(float a, float b, float k) {
    if (k <= 0.0001) return min(a, b);
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

/// Polynomial smooth subtraction: carves `b`'s volume out of `a`. k <= 0
/// degenerates to a hard max(a, -b).
float gfx_sdf_smooth_subtract(float a, float b, float k) {
    if (k <= 0.0001) return max(a, -b);
    float h = clamp(0.5 - 0.5 * (b + a) / k, 0.0, 1.0);
    return mix(a, -b, h) + k * h * (1.0 - h);
}

/// Polynomial smooth intersection. k <= 0 degenerates to a hard max(a, b).
float gfx_sdf_smooth_intersect(float a, float b, float k) {
    if (k <= 0.0001) return max(a, b);
    float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) + k * h * (1.0 - h);
}

/// Ray/AABB slab test in world space against an arbitrary (non-centered)
/// box [bmin, bmax]. Returns [t_enter, t_exit]; t_enter > t_exit means no
/// intersection.
///
/// Same axis-parallel-ray guard as gfx_fog_box_intersect() (gfx/fog.glsl,
/// see that function's doc for the full instability it avoids): when `rd`'s
/// component on some axis is near zero, a naive 1/rd division can produce a
/// spurious large-but-finite t instead of the correct signed infinity, which
/// then corrupts the combined min/max bound instead of being cleanly
/// rejected. Adapted here from gfx_fog_box_intersect()'s centered-extent form
/// to this function's min/max form, since the SDF renderer's world AABB is
/// not generally centered on the ray-space origin the way a Volume's
/// local-space box is.
vec2 gfx_sdf_aabb_intersect(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {
    bvec3 parallel = lessThan(abs(rd), vec3(1e-8));
    vec3  safe_rd  = mix(rd, vec3(1.0), parallel);
    vec3  inv_rd   = 1.0 / safe_rd;
    vec3  t0 = (bmin - ro) * inv_rd;
    vec3  t1 = (bmax - ro) * inv_rd;
    vec3  tmin = min(t0, t1);
    vec3  tmax = max(t0, t1);

    bvec3 inside_slab = bvec3(
        ro.x >= bmin.x && ro.x <= bmax.x,
        ro.y >= bmin.y && ro.y <= bmax.y,
        ro.z >= bmin.z && ro.z <= bmax.z);
    tmin = mix(tmin, mix(vec3(1e30), vec3(-1e30), inside_slab), parallel);
    tmax = mix(tmax, mix(vec3(-1e30), vec3(1e30), inside_slab), parallel);

    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

/// Reconstructs a world-space ray from a clip-space (NDC) xy coordinate by
/// unprojecting the near (z=0) and far (z=1, Vulkan depth range) plane
/// points through `inv_view_proj`. Works unmodified under both perspective
/// and orthographic projections -- there is no perspective-specific term
/// here, only two unprojections and a subtract, so this is what lets the
/// same SDF shaders run under the pixel-art pipeline's orthographic camera
/// and a perspective one with no branch.
void gfx_sdf_ray_from_clip(mat4 inv_view_proj, vec2 ndc, out vec3 ro, out vec3 rd) {
    vec4 near_h = inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec4 far_h  = inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 near_p = near_h.xyz / near_h.w;
    vec3 far_p  = far_h.xyz / far_h.w;
    ro = near_p;
    rd = normalize(far_p - near_p);
}

#endif // GFX_SDF_GLSL
