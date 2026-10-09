#version 450

// Topdown mode's toon cloud layer (TopdownCloudPass): puffy, flat-shaded clouds floating at a
// fixed world height between a zoomed-out topdown camera and the ground, and their shadows on
// the ground. Drawn over the lit, fogged and translucent scene with premultiplied alpha:
//   rgb = cloud colour * coverage, a = 1 - (1 - coverage) * (1 - shadow)
// so one draw both lays the clouds over the scene and darkens the ground in their shadows.
//
// The layer is a heightfield: each puff's top stands up to `thickness` above the layer's base,
// its height following how far the cloud field (the sky's weather map for cloud groups, carved
// by the Perlin-Worley shape volume into puffs) rises above the coverage threshold. A ray
// marches the slab in fixed steps and refines the surface by bisection -- no jitter, so the
// silhouettes are crisp and perfectly still -- and the edge is anti-aliased from the field's
// screen-space derivative, one pixel wide at any zoom. Shading is cel-style: the sun's N.L quantised into
// bands over the sky's ambient, with a darker rim. The clouds fade in as the camera climbs above
// the layer (fade_start .. fade_end), so they show only when zoomed out; the shadows stay.

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
layout(set = 2, binding = 2) uniform sampler2D u_weather;    // the sky clouds' weather map, repeat
layout(set = 2, binding = 3) uniform sampler3D u_shape;      // the sky clouds' shape volume, repeat

layout(push_constant) uniform TopdownParams {
    mat4 inv_view_proj;   // this frame's (jittered under TAA, like every surface)
    vec4 layer;           // x = height (world z), y = puff size (m), z = thickness (m), w = opacity
    vec4 look;            // x = fade start, y = fade end (camera height above the layer), z = shadow strength, w = light bands
    vec4 wind;            // xy = drift offset (m), z = coverage 0..1, w = outline 0..1
    vec4 extra;           // x = time (s)
} pc;

const float PI = 3.14159265;

// The cloud field at a world xy, as a signed margin: > 0 inside a cloud (how far the field rises
// above the coverage threshold), < 0 in clear sky. Crossing zero is the cloud's edge.
float margin(vec2 xy) {
    vec2 p = xy + pc.wind.xy;
    float size = pc.layer.y;
    float cov = pc.wind.z;
    if (cov < 0.005) return -1.0;
    // Cloud groups from the weather map (an even spread over [0, 1], so about `cov` of the
    // ground exceeds 1 - cov), their edges broken into puffs by the shape volume's Perlin-Worley.
    float groups = textureLod(u_weather, p / (size * 18.0), 0.0).r;
    float puffs = textureLod(u_shape, vec3(p / (size * 3.2), 0.37 + pc.extra.x * 0.0015), 0.0).r;
    return groups + (puffs - 0.75) * 0.5 - (1.0 - cov);   // 0.75: the puffs' mean
}

// The puff's top above the layer's base for a margin: a dome rising over the cloud's edge band.
float top_of(float m) { return sqrt(clamp(m / 0.3, 0.0, 1.0)) * pc.layer.z; }

void main() {
    vec3 N = texture(g_normal, in_uv).rgb;
    bool sky = dot(N, N) < 0.001;
    vec3 ro = camera.camera_pos;
    vec3 ndc = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 wp = pc.inv_view_proj * vec4(ndc, 1.0);
    vec3 rd = normalize(wp.xyz / wp.w - ro);
    float t_scene = 1e9;
    vec3 P = vec3(0.0);
    if (!sky) {
        P = texture(g_position, in_uv).xyz;
        t_scene = distance(ro, P);
    }

    // --- Shadow on the surface: the field where the sun's ray through P crosses the layer ---
    float shadow = 0.0;
    vec3 L = normalize(-lights.dir_direction.xyz);
    bool has_sun = lights.light_counts.x > 0u && L.z > 0.05;
    float base = pc.layer.x - 0.5 * pc.layer.z;
    if (!sky && has_sun && pc.look.z > 0.0 && P.z < base) {
        vec3 q = P + L * ((pc.layer.x - P.z) / L.z);
        // A low sun stretches the shadows into long diffuse smears -- and across a wall the
        // lookup would sweep the field in bands -- so they fade out as it sets.
        shadow = smoothstep(0.0, 0.08, margin(q.xy)) * pc.look.z * smoothstep(0.15, 0.4, L.z);
    }

    // --- The cloud surface along the view ray ---
    float fade = smoothstep(pc.look.x, pc.look.y, ro.z - pc.layer.x);
    float alpha = 0.0;
    float m_hit = -1.0;   // the margin where the ray met a cloud (-1: no cloud)
    vec3 col = vec3(0.0);
    float top = base + pc.layer.z;
    if (fade > 0.0 && rd.z < -0.02 && ro.z > top) {
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
                float m = margin(p.xy);
                if (m > 0.0 && p.z - base < top_of(m)) { tb = t; break; }
                ta = t;
            }
            if (tb > 0.0) {
                for (int i = 0; i < 6; ++i) {
                    float tm = 0.5 * (ta + tb);
                    vec3 p = ro + rd * tm;
                    float m = margin(p.xy);
                    if (m > 0.0 && p.z - base < top_of(m)) tb = tm; else ta = tm;
                }
                vec3 h = ro + rd * tb;
                m_hit = max(margin(h.xy), 0.0);
                // The surface normal from the heightfield's slope.
                float e = pc.layer.y * 0.15;   // wide: smooth normals give clean bands, not islands
                float hx = top_of(margin(h.xy + vec2(e, 0.0))) - top_of(margin(h.xy - vec2(e, 0.0)));
                float hy = top_of(margin(h.xy + vec2(0.0, e))) - top_of(margin(h.xy - vec2(0.0, e)));
                vec3 n = normalize(vec3(-hx, -hy, 2.0 * e));

                // Cel shading: the sun's N.L, wrapped and stepped into bands, over the sky's ambient.
                float ndl = has_sun ? clamp(dot(n, L) * 0.6 + 0.4, 0.0, 1.0) : 0.0;
                float bands = pc.look.w;
                if (bands >= 1.0) ndl = floor(ndl * bands + 0.5) / bands;
                vec3 sun = lights.dir_color.rgb * lights.dir_direction.w;
                vec3 ambient = mix(lights.sky_horizon.rgb, lights.sky_zenith.rgb, clamp(n.z, 0.0, 1.0));
                const float ALBEDO = 0.92;
                col = ALBEDO * (sun * ndl / PI + ambient);
                // A darker rim band along the puff's edge, toon-ink style.
                float rim = 1.0 - smoothstep(0.02, 0.06, m_hit);
                col *= 1.0 - 0.45 * pc.wind.w * rim;
                alpha = pc.layer.w * fade;
            }
        }
    }
    // Anti-aliased silhouettes: the margin's screen-space rate turns it into a distance in pixels
    // (an SDF-style edge, one pixel wide at any zoom). Outside the branch, so every pixel of the
    // quad computes the derivative.
    float fw = max(fwidth(m_hit), 1e-4);
    alpha *= clamp(m_hit / fw + 0.5, 0.0, 1.0);

    float cover = 1.0 - (1.0 - alpha) * (1.0 - shadow);
    if (cover <= 0.0) discard;
    out_color = vec4(col * alpha, cover);
}
