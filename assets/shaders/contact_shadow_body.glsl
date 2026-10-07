#ifndef TOY_CONTACT_SHADOW_BODY_GLSL
#define TOY_CONTACT_SHADOW_BODY_GLSL

// contact_shadow_body.glsl -- toy_contact_shadow(), the screen-space contact-shadow march that
// contact_shadow.frag runs once per pixel into its own buffer (ContactShadowPass), which the
// lighting pass then reads.
//
// The shape is Unreal's "screen space contact shadows": ONE ray per pixel toward the light, a
// handful of steps, and at each step ONE fetch of the scene depth buffer compared against the
// ray's own depth with a thickness tolerance that dilates with the step length. The previous
// version marched four cone rays with a 3x3 tent of position-and-normal fetches per step (~580
// texture reads per pixel, ~20 ms at 1080p); this one reads `steps` depth texels. Its grain is
// left to the shared temporal resolve that follows the march (ssr_resolve.frag against the
// TemporalHistoryPass count), which is exactly the buffer this pass exists to give it.
//
// A "body" file: the includer must, BEFORE including this file, already have declared:
//   - `camera`         -- CameraUBO { mat4 view; mat4 proj; vec3 camera_pos; }.
//   - `lights`         -- the LightUBO block (contact_params, contact_soft_params,
//                         dir_shadow_extra.w -- see light_data.h's C++ doc).
//   - `u_scene_depth`  -- sampler2D over the rasterised scene depth (NEAREST).
//   - #include <gfx/ssr_common.glsl> (for ssr_texel_world_size(), ssr_ndc_to_uv()).
//
// CALLER CONTRACT: every vector is in VIEW space. dpdx/dpdy are the screen-space derivatives of
// the reconstructed view position, taken BEFORE any non-uniform branch (see contact_shadow.frag);
// they feed the geometric normal and the jitter-stable march origin.

/// Linear view distance (positive, world units) of a stored depth-buffer value, for either
/// projection kind. RH_ZO perspective: d = -c22 - c32 / z_view; orthographic: d = c22 z + c32.
float toy_contact_linear_depth(float d) {
    float c22 = camera.proj[2][2];
    float c32 = camera.proj[3][2];
    if (camera.proj[2][3] != 0.0) return c32 / (d + c22);
    return -(d - c32) / c22;
}

// Returns the RAW geometric occlusion (0..1) of the march -- NOT scaled by
// lights.contact_params.x (strength) or lights.dir_shadow_extra.x (per-light darkness); the
// lighting pass max()-combines and scales those in.
//
// Everything is VIEW space. P is this pixel's surface point reconstructed from the depth buffer
// (exact, unlike the RGBA16F G-buffer position, whose quantisation at world coordinates of a few
// hundred units is larger than the depth tolerance below and starts the ray under its own
// surface). Ng is the GEOMETRIC normal from P's screen derivatives: the shading normal carries
// normal-map relief that the depth buffer does not, and testing a ray against flat depth with a
// bumpy normal makes a brick wall shadow its own mortar lines. N is the shading normal, used
// only to skip pixels the light cannot reach anyway.
float toy_contact_shadow(vec3 P, vec3 N, vec3 Ng, vec3 L, vec3 dpdx, vec3 dpdy) {
    if (lights.contact_params.x <= 0.0) return 0.0;
    if (dot(N, L) <= 0.0) return 0.0;

    float ndl = dot(Ng, L);
    // Grazing fade: as N.L -> 0 the ray travels nearly parallel to the surface it started
    // from, so every depth comparison is against that same surface.
    float graze = smoothstep(0.0, 0.15, ndl);
    if (graze <= 0.0) return 0.0;

    ivec2 gsize = textureSize(u_scene_depth, 0);
    vec3  V     = normalize(-P);

    // Jitter-stable march origin: P is the surface the JITTERED raster saw at this pixel's
    // centre, which on a grazing receiver slides along the surface every frame. The perspective
    // jitter is recoverable from the projection (apply_taa_jitter_ adds it to proj[2][0]/[2][1])
    // and dpdx/dpdy are the surface's footprint per pixel; together they slide the origin back
    // to the point under the unjittered texel centre.
    float px_raw = ssr_texel_world_size(P.z, abs(camera.proj[1][1]), float(gsize.y));
    vec3 origin = P;
    if (camera.proj[2][3] != 0.0) {
        vec2 jitter_ndc = vec2(-camera.proj[2][0], -camera.proj[2][1]);
        vec3 corr = dpdx * (0.5 * float(gsize.x) * jitter_ndc.x)
                  - dpdy * (0.5 * float(gsize.y) * jitter_ndc.y);
        // In-plane only, and capped: where the derivative quad straddles a depth edge the
        // "footprint" is garbage metres of cross-surface distance, not slope.
        corr -= Ng * dot(Ng, corr);
        float cap = px_raw * 4.0 / max(abs(dot(Ng, V)), 0.05);
        if (dot(corr, corr) < cap * cap) origin += corr;
    }

    float px_world = ssr_texel_world_size(origin.z, abs(camera.proj[1][1]), float(gsize.y));

    // Start offset sized to the receiver's own screen texel, so the first sample always clears
    // the surface it came from; a grazing ray must travel further to gain the same height.
    float bias = max(px_world * 2.0 / max(ndl, 0.15), 0.002);

    // Clamp the march's SCREEN extent: a fixed world length spans hundreds of texels up close
    // and less than one at range. 64 px is the same order as SSAO's ssao_max_radius_px cap.
    float ray_len   = min(lights.contact_params.y, px_world * 64.0);
    int   steps     = int(max(lights.contact_params.w, 1.0));
    float thickness = max(lights.contact_params.z, 1e-4);

    // Depth difference a sample on the receiver's OWN plane can show from depth quantisation and
    // the ray's sub-texel position: about a texel's world size, stretched by how obliquely the
    // camera sees that plane. Anything closer than this behind the stored surface is the
    // receiver itself, not an occluder.
    float depth_eps = max(px_world * (1.0 + 1.0 / max(abs(dot(Ng, V)), 0.1)), 0.002);

    // Project the march with the UNJITTERED projection so the fetch path does not slide with
    // TAA's sub-pixel offset (the depth buffer itself is jittered by under a texel, which the
    // tolerance above absorbs).
    mat4 proj_march = camera.proj;
    if (camera.proj[2][3] != 0.0) {
        proj_march[2][0] = 0.0;
        proj_march[2][1] = 0.0;
    }

    // Per-pixel step phase: interleaved gradient noise, rotated by the golden ratio each frame
    // (contact_soft_params.y) so the temporal resolve integrates the comb into a smooth result.
    // The CPU holds that rotation at 0 when the resolve is off: with nothing to average it, a
    // rotating comb would only make a still image shimmer.
    float phase = fract(fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                                  0.00583715 * gl_FragCoord.y))
                        + lights.contact_soft_params.y * 0.61803398875);

    // Soft contact shadows (soft_shadows on): a blocker found far along the ray covers less of
    // the sun's disk than one touching the receiver, so a hit's weight falls with its distance
    // along the ray -- the PCSS relation, in one ray. cone_tan is the sun's angular size
    // (shadow_pcss_light_size); scaled up because the march is centimetres long, over which the
    // true angular penumbra would be invisible.
    float soft = clamp(lights.contact_soft_params.x * 20.0, 0.0, 0.75);

    float occ = 0.0;
    float prev_t = 0.0;
    for (int s = 0; s < steps; ++s) {
        // Quadratic spacing concentrates samples near the contact point, where this effect
        // lives, and spends least on the far end, where screen-space data is least reliable.
        float f = (float(s) + phase) / float(steps);
        float t = ray_len * f * f;
        vec3  p_view = origin + L * (t + bias);

        vec4 clip = proj_march * vec4(p_view, 1.0);
        if (clip.w <= 0.0) break;
        vec2 uv = ssr_ndc_to_uv(clip.xy / clip.w);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;

        ivec2 px = clamp(ivec2(uv * vec2(gsize)), ivec2(0), gsize - 1);
        float d  = texelFetch(u_scene_depth, px, 0).r;
        // Dilated thickness: a step that crosses `dz` of view depth can land that far behind a
        // thin occluder without having passed through empty space, so the acceptance window
        // grows with the step's own depth span (Unreal's compare tolerance).
        float dz  = abs(L.z) * (t - prev_t);
        prev_t = t;
        if (d >= 1.0) continue;   // sky: nothing there to occlude

        float delta = (-p_view.z) - toy_contact_linear_depth(d);   // > 0: ray behind the surface
        // The window opens above the self-intersection tolerance rather than competing with it:
        // at a low render resolution one texel spans more depth than the occluder thickness, and
        // a window of [eps, thickness] would then accept nothing at all.
        float tol   = depth_eps + max(thickness, 2.0 * dz);
        if (delta > depth_eps && delta < tol) {
            // Fade with distance along the ray and toward the screen edge (where the depth the
            // sample read may belong to a surface that only partly reaches into frame).
            vec2  e   = smoothstep(vec2(0.0), vec2(0.08), uv) * smoothstep(vec2(1.0), vec2(0.92), uv);
            float hit = (1.0 - smoothstep(0.6, 1.0, f)) * e.x * e.y * (1.0 - soft * f);
            occ = max(occ, hit);
        }
    }
    return occ * graze;
}

#endif // TOY_CONTACT_SHADOW_BODY_GLSL
