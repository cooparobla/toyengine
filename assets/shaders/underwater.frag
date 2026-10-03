#version 450

// underwater.frag -- the underwater look (see toyengine/render/passes/underwater_pass.h).
//
// For every pixel whose view ray STARTS below the water level (its near-plane point), the ray's
// path through water -- up to the geometry it hits, or up to where it leaves through the surface
// plane -- is absorbed per channel (red first, as in real water) and fogged toward the water's
// in-scatter colour. Submerged geometry also gets caustics, fading with depth. Pixels whose ray
// starts above the level (a camera straddling the waterline) are passed through untouched.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;

layout(push_constant) uniform UnderwaterParams {
    mat4 inv_view_proj;
    vec4 camera_time;  // xyz = camera position, w = time
    vec4 water;        // x = level, y = density (1/m), z = caustics, w = enabled
    vec4 fog_color;    // rgb, w = shimmer
    vec4 absorption;   // rgb per metre, w = light intensity
} u;

/// Tileable animated caustic pattern (sum of distorted sines), in [0, ~1].
float caustic(vec2 p, float t) {
    vec2 q = mod(p * 6.28318, 6.28318) - 250.0;
    vec2 i = q;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 4; ++n) {
        float tt = t * (1.0 - 3.5 / float(n + 1));
        i = q + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(vec2(q.x / (sin(i.x + tt) / inten), q.y / (cos(i.y + tt) / inten)));
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    return clamp(pow(abs(c), 8.0), 0.0, 1.0);
}

vec3 unproject(vec2 uv, float ndc_z) {
    vec4 w = u.inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndc_z, 1.0);
    return w.xyz / w.w;
}

void main() {
    if (u.water.w < 0.5) {
        out_color = vec4(texture(scene_color, in_uv).rgb, 1.0);
        return;
    }

    const float level = u.water.x;
    const float t     = u.camera_time.w;

    // The ray from this pixel's near-plane point (NOT the camera position: a camera straddling
    // the surface must split the image at the waterline).
    vec3 near_p = unproject(in_uv, 0.0);
    vec3 far_p  = unproject(in_uv, 1.0);
    vec3 dir    = normalize(far_p - near_p);
    if (near_p.z >= level) {
        out_color = vec4(texture(scene_color, in_uv).rgb, 1.0);
        return;
    }

    // A little refractive shimmer, then the (shimmered) scene.
    vec2 wobble = vec2(sin(in_uv.y * 38.0 + t * 1.9), cos(in_uv.x * 31.0 + t * 1.6)) * 0.0018 * u.fog_color.w;
    vec2 uv = clamp(in_uv + wobble, vec2(0.0), vec2(1.0));
    vec3 color = texture(scene_color, uv).rgb;

    // Path length through water: to the geometry, or to the surface plane, whichever is first.
    vec3  N      = texelFetch(g_normal_metallic, ivec2(uv * vec2(textureSize(g_normal_metallic, 0))), 0).rgb;
    vec3  gpos   = texelFetch(g_position_roughness, ivec2(uv * vec2(textureSize(g_position_roughness, 0))), 0).rgb;
    bool  is_sky = dot(N, N) < 0.001;
    float t_geo  = is_sky ? 1e6 : length(gpos - near_p);
    float t_exit = dir.z > 1e-4 ? (level - near_p.z) / dir.z : 1e6;
    float dist   = min(min(t_geo, t_exit), 400.0);

    // Caustics on submerged, upward-facing geometry, fading with depth below the surface.
    if (!is_sky && t_geo <= t_exit && gpos.z < level) {
        float below = level - gpos.z;
        float c = caustic(gpos.xy * 0.32, t * 0.6) + 0.6 * caustic(gpos.xy * 0.55 + 3.7, t * 0.8);
        color *= 1.0 + c * u.water.z * u.absorption.w * max(N.z, 0.0) * exp(-below * 0.12) * 3.0;
    }

    // Absorption (per channel) and in-scatter. The in-scattered light comes from above: it is
    // brightest looking up toward the surface, darkest looking down into the depths, and dims
    // overall with the camera's own depth.
    vec3  transmit = exp(-(u.absorption.rgb + vec3(u.water.y)) * dist);
    float cam_depth = max(level - near_p.z, 0.0);
    float updown   = mix(0.35, 1.2, smoothstep(-0.8, 0.8, dir.z));
    vec3  scatter  = u.fog_color.rgb * u.absorption.w * 0.6 * updown * exp(-cam_depth * 0.06);
    color = color * transmit + scatter * (vec3(1.0) - transmit);

    out_color = vec4(color, 1.0);
}
