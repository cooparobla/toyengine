#version 450

// The flat (toon) cloud layer (render cloud_type: flat; CloudOverlayPass): puffy, flat-shaded
// clouds at a fixed world height, drawn over the lit, fogged and translucent scene with
// premultiplied alpha (rgb = cloud colour * coverage, a = coverage). Their shadows are not drawn
// here: the cloud shadow map (cloud_shadow_flat.frag) darkens the sun's light wherever it falls.
//
// From ABOVE the layer is a heightfield: each puff's top stands up to the puff height above the
// layer's base, following how far the field (flat_cloud_field.glsl) rises above the coverage
// threshold. A ray marches the slab in fixed steps and refines the surface by bisection -- no
// jitter, so the silhouettes are crisp and perfectly still. Shading is cel-style: the sun's N.L
// quantised into bands over the sky's ambient, with a darker rim.
// From BELOW it is a flat deck at the base: the field where the ray meets it, shaded darker where
// the cloud is thicker (less light through), in the same bands, fading out toward the horizon
// before the puffs shrink below a pixel.
// Either way the edge is anti-aliased from the field's screen-space derivative, one pixel wide
// at any zoom, and the layer fades with the camera's height (render cloud_camera_fade).

#include <gfx/spot_light.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

#include "light_ubo_body.glsl"

layout(set = 2, binding = 0) uniform sampler2D g_normal;     // nearest
layout(set = 2, binding = 1) uniform sampler2D g_position;   // nearest: world position
layout(set = 2, binding = 2) uniform sampler2D u_weather;    // the volumetric clouds' weather map, repeat
layout(set = 2, binding = 3) uniform sampler3D u_shape;      // the volumetric clouds' shape volume, repeat

#define FLAT_UBO_SET 2
#define FLAT_UBO_BINDING 4
#include "flat_cloud_field.glsl"

layout(push_constant) uniform FlatParams {
    mat4 inv_view_proj;   // this frame's (jittered under TAA, like every surface)
} pc;

const float PI = 3.14159265;
const float ALBEDO = 0.92;

float bands_of(float v) {
    float bands = fc.look.y;
    return bands >= 1.0 ? floor(v * bands + 0.5) / bands : v;
}

void main() {
    vec3 N = texture(g_normal, in_uv).rgb;
    bool sky = dot(N, N) < 0.001;
    vec3 ro = camera.camera_pos;
    vec3 ndc = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 wp = pc.inv_view_proj * vec4(ndc, 1.0);
    vec3 rd = normalize(wp.xyz / wp.w - ro);
    float t_scene = sky ? 1e30 : distance(ro, texture(g_position, in_uv).xyz);

    vec3 L = normalize(-lights.dir_direction.xyz);
    bool has_sun = lights.light_counts.x > 0u && L.z > 0.0;
    vec3 sun = has_sun ? lights.dir_color.rgb * lights.dir_direction.w : vec3(0.0);
    float base = fc.layer.x;
    float top = base + fc.layer.y;
    float fade = fc.look.w;

    float alpha = 0.0;
    float m_hit = -1.0;   // the margin where the ray met a cloud (-1: no cloud)
    vec3 col = vec3(0.0);
    if (fade > 0.0 && rd.z < -0.02 && ro.z > top) {
        // --- From above: the heightfield ---
        float t0 = (top - ro.z) / rd.z;
        float t1 = min((base - ro.z) / rd.z, t_scene);
        if (t1 > t0) {
            // March down through the slab until the ray is below a puff's top, then bisect.
            const int STEPS = 24;
            float dt = (t1 - t0) / float(STEPS);
            float ta = t0, tb = -1.0;
            for (int i = 1; i <= STEPS; ++i) {
                float t = t0 + dt * float(i);
                vec3 p = ro + rd * t;
                float m = flat_margin(p.xy);
                if (m > 0.0 && p.z - base < flat_top_of(m)) { tb = t; break; }
                ta = t;
            }
            if (tb > 0.0) {
                for (int i = 0; i < 6; ++i) {
                    float tm = 0.5 * (ta + tb);
                    vec3 p = ro + rd * tm;
                    float m = flat_margin(p.xy);
                    if (m > 0.0 && p.z - base < flat_top_of(m)) tb = tm; else ta = tm;
                }
                vec3 h = ro + rd * tb;
                m_hit = max(flat_margin(h.xy), 0.0);
                // The surface normal from the heightfield's slope.
                float e = fc.layer.z * 0.15;   // wide: smooth normals give clean bands, not islands
                float hx = flat_top_of(flat_margin(h.xy + vec2(e, 0.0))) - flat_top_of(flat_margin(h.xy - vec2(e, 0.0)));
                float hy = flat_top_of(flat_margin(h.xy + vec2(0.0, e))) - flat_top_of(flat_margin(h.xy - vec2(0.0, e)));
                vec3 n = normalize(vec3(-hx, -hy, 2.0 * e));

                // Cel shading: the sun's N.L, wrapped and stepped into bands, over the sky's ambient.
                float ndl = has_sun ? bands_of(clamp(dot(n, L) * 0.6 + 0.4, 0.0, 1.0)) : 0.0;
                vec3 ambient = mix(lights.sky_horizon.rgb, lights.sky_zenith.rgb, clamp(n.z, 0.0, 1.0));
                col = ALBEDO * (sun * ndl / PI + ambient);
                // A darker rim band along the puff's edge, toon-ink style.
                float rim = 1.0 - smoothstep(0.02, 0.06, m_hit);
                col *= 1.0 - 0.45 * fc.look.z * rim;
                alpha = fc.layer.w * fade;
            }
        }
    } else if (fade > 0.0 && rd.z > 0.01 && ro.z < base) {
        // --- From below: the deck's underside ---
        float t = (base - ro.z) / rd.z;
        // Out to where a puff shrinks toward a pixel (the horizon), the deck fades out.
        float far = 1.0 - smoothstep(fc.layer.z * 12.0, fc.layer.z * 40.0, t);
        if (t < t_scene && far > 0.0) {
            vec3 h = ro + rd * t;
            m_hit = max(flat_margin(h.xy), 0.0);
            if (m_hit > 0.0) {
                // Thicker cloud lets less of the sun through: thin edges glow, cores go grey.
                float thick = clamp(m_hit / 0.3, 0.0, 1.0);
                float through = has_sun ? bands_of(clamp(1.0 - 0.75 * sqrt(thick), 0.0, 1.0)) : 0.0;
                vec3 ambient = mix(lights.sky_horizon.rgb, lights.sky_ground.rgb, 0.35);
                col = ALBEDO * (sun * (0.15 + 0.5 * through) / PI + ambient * mix(1.0, 0.6, thick));
                float rim = 1.0 - smoothstep(0.02, 0.06, m_hit);
                col *= 1.0 + 0.25 * fc.look.z * rim;   // seen from below, the thin rim is the bright part
                alpha = fc.layer.w * fade * far;
            }
        }
    }
    // Anti-aliased silhouettes: the margin's screen-space rate turns it into a distance in pixels
    // (an SDF-style edge, one pixel wide at any zoom). Outside the branches, so every pixel of the
    // quad computes the derivative.
    float fw = max(fwidth(m_hit), 1e-4);
    alpha *= clamp(m_hit / fw + 0.5, 0.0, 1.0);

    if (alpha <= 0.0) discard;
    out_color = vec4(col * alpha, alpha);
}
