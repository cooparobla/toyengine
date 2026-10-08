#version 450

// ParticlePass's fragment stage -- see toyengine/render/passes/particle_pass.h.
//
// The look sits between realism and stylization, the same register as the water shader: real
// lighting (sun, sky, nearby point lights) and soft depth intersection, but shapes with crisp,
// noise-broken silhouettes and lighting that can be quantized into toon bands. Every sprite is
// procedural -- a signed distance in quad space, broken up by scrolling value noise -- so effects
// need no texture authoring; `sprite: texture` samples an albedo map / flipbook instead.
//
// Output is PREMULTIPLIED: rgb * a, with alpha scaled by (1 - additive), so a single
// premultiplied-blend pipeline covers alpha-blended smoke and additive fire alike.
//
// Light, for `lit` particles: the sky gradient (ambient), the sun with a half-Lambert wrap and
// the point and spot lights the same way, each shadowed (`receive_shadows`: one hard tap of the
// sun's cascade atlas, the local-light atlas for shadowed lamps), plus Henyey-Greenstein
// FORWARD scattering of every light toward the eye (`scatter`, `scatter_anisotropy`) -- what
// makes rain glint against a street lamp or a low sun and backlit smoke glow.
//
// The same shader also draws the TAA REACTIVE MASK (extra.x == 1): ParticlePass's second
// pipeline renders each `reactive` batch again into an R8 target, writing coverage * reactive
// and depth-testing by hand against the Hi-Z copy of the scene depth (that pass has no depth
// attachment); taa.frag then trusts the current frame wherever the mask is set, so fast thin
// particles never smear into streaks.

#include <gfx/spot_light.glsl>
#include <light_ubo_body.glsl>
#include <gfx/sky.glsl>
#include <gfx/depth.glsl>
#include <gfx/shadow_sampling.glsl>

// Set 4: the shadow maps -- the same set (and names) the forward transparent shading binds at 2;
// pixel_shadow_body.glsl reads them through these exact names.
layout(set = 4, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 4, binding = 1) uniform sampler2DShadow local_shadow_atlas;
layout(set = 4, binding = 3) uniform sampler2D dir_shadow_map_raw;
#include "pixel_shadow_body.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec3 v_world;
layout(location = 3) in vec4 v_misc;      // x age, y random, z frame, w view depth
layout(location = 4) in vec3 v_right;
layout(location = 5) in vec3 v_up;
layout(location = 6) in vec3 v_normal;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 2: the Hi-Z pyramid -- mip 0 is a copy of the opaque G-buffer depth (raw [0,1]), at the
// render extent. Soft particles read it rather than the depth attachment this pass has bound.
layout(set = 2, binding = 0) uniform sampler2D u_hiz_map;
// Set 3: the material set; only the albedo slot is used (the sprite texture / flipbook).
layout(set = 3, binding = 1) uniform sampler2D u_albedo_map;

layout(push_constant) uniform ParticlePC {
    vec4 mode;     // x render mode, y sprite, z flipbook cols, w flipbook rows
    vec4 shading;  // x lit, y toon bands, z emissive, w additive
    vec4 shape;    // x softness, y soft distance, z camera fade, w aspect
    vec4 stretch;  // x stretch speed, y stretch length, z distortion, w time
    vec4 misc;     // x opacity, y has texture, z pivot, w is perspective
    vec4 depth;    // x near, y far, zw 1 / render extent
    vec4 ambient;  // x sky/ambient scale, y receive shadows
    vec4 extra;    // x pass (0 colour, 1 TAA reactive mask), y reactive, z scatter, w scatter anisotropy
} pc;

layout(location = 0) out vec4 out_color;

const int SPRITE_SOFT    = 0;
const int SPRITE_CIRCLE  = 1;
const int SPRITE_PUFF    = 2;
const int SPRITE_FLAME   = 3;
const int SPRITE_SPARK   = 4;
const int SPRITE_RING    = 5;
const int SPRITE_STAR    = 6;
const int SPRITE_LEAF    = 7;
const int SPRITE_TEXTURE = 8;

// --- noise ------------------------------------------------------------------------------

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm2(vec2 p) {
    return vnoise(p) * 0.65 + vnoise(p * 2.13 + 17.1) * 0.35;
}

// --- shapes -----------------------------------------------------------------------------

// Coverage from a signed distance (negative inside): a crisp, antialiased cel edge at softness
// 0, a long feather at 1.
float coverage(float d, float softness) {
    float aa = max(fwidth(d) * 0.75, 1e-3);
    float w = aa + softness * 0.6;
    return clamp(0.5 - d / w, 0.0, 1.0);
}

float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// The quantized-step operator used for every toon effect: `bands` levels, 0 = smooth.
float toon(float x, float bands) {
    if (bands < 1.5) return x;
    return floor(x * bands + 0.5) / bands;
}

// The sun's shadow at a particle: ONE hard tap of the cascade atlas (no PCF: a particle is not a
// surface, and fill-rate is the cost that matters here), on the first cascade holding the point.
float particle_dir_shadow(vec3 world_pos) {
    int count = int(lights.dir_cascade_info.x);
    vec2 uv;
    for (int c = 0; c < 4; ++c) {
        if (c >= count) break;
        if (!toy_csm_contains(c, world_pos, 0.0, uv)) continue;
        vec3 coords = toy_csm_atlas_coords(c, world_pos);
        float bias = 2.0 * lights.dir_cascade_depth_bias[c];
        float fade = 1.0;
        if (lights.dir_shadow_fade.w > 0.0) {
            float d = length(world_pos - lights.dir_shadow_fade.xyz);
            fade = 1.0 - smoothstep(lights.dir_shadow_fade_params.x, lights.dir_shadow_fade.w, d);
        }
        return gfx_shadow_dir_hard(dir_shadow_map, coords, bias) * fade * lights.dir_shadow_extra.x;
    }
    return 0.0;
}

void main() {
    int   sprite    = int(pc.mode.y + 0.5);
    float softness  = pc.shape.x;
    float distort   = pc.stretch.z;
    float t         = pc.stretch.w;
    float age       = v_misc.x;
    float rnd       = v_misc.y;
    float bands     = pc.shading.y;
    vec2  uv        = v_uv;
    float r         = length(uv);

    vec3  albedo = v_color.rgb;
    float a      = 0.0;
    float heat   = 0.0;          // flame / spark core, 0..1
    vec3  N      = v_normal;     // lighting normal
    bool  sphere_normal = false;

    if (sprite == SPRITE_SOFT) {
        float g = clamp(1.0 - r * r, 0.0, 1.0);
        a = g * g;
        a = mix(a, smoothstep(0.0, 0.35, a), 1.0 - softness);
    } else if (sprite == SPRITE_CIRCLE) {
        a = coverage(r - 0.86, softness * 0.4);
        albedo *= mix(1.0, 1.18, smoothstep(0.55, 0.8, r));   // light rim
        sphere_normal = true;
    } else if (sprite == SPRITE_PUFF) {
        // A lumpy ball: radius broken by noise that drifts with age, so a rising puff churns.
        vec2 q = uv * 1.35 + vec2(rnd * 37.0, rnd * 11.0);
        float n = fbm2(q + vec2(0.0, -age * 1.4 - t * 0.15));
        float rad = 0.80 + (n - 0.5) * 0.65 * distort;
        // A second, smaller lobe pass carves the silhouette into billows.
        float lobes = vnoise(vec2(atan(uv.y, uv.x) * 1.6 + rnd * 9.0, age * 2.0));
        rad -= (lobes - 0.5) * 0.18 * distort;
        // Denser in the middle than at the rim, so overlapping puffs blend into one body of smoke
        // instead of stacking up as flat discs.
        a = coverage(r - rad, softness) * mix(0.45, 1.0, smoothstep(rad, 0.0, r));
        sphere_normal = true;
    } else if (sprite == SPRITE_FLAME) {
        // A teardrop tongue: a round bulb low down, smooth-unioned into a cone to the tip, its
        // sides licking with noise that grows toward the tip and scrolls upward.
        vec2 p = uv;
        float lick = (vnoise(vec2(p.y * 2.2 - t * 3.1, rnd * 13.0)) - 0.5) * distort * (p.y + 1.0) * 0.45;
        p.x += lick;
        float bulb = length(vec2(p.x, (p.y + 0.38) * 1.05)) - 0.58;
        float tipy = clamp((p.y + 0.38) / 1.38, 0.0, 1.0);
        float cone = abs(p.x) - 0.58 * pow(1.0 - tipy, 1.35);
        cone = max(cone, -(p.y + 0.38));
        float d = smin(bulb, p.y > -0.38 ? cone : bulb, 0.18);
        d = max(d, p.y - 0.98);
        float erosion = fbm2(vec2(p.x * 2.4, p.y * 1.6 - t * 2.6 - age * 2.0) + rnd * 7.0);
        d += (erosion - 0.5) * 0.42 * distort + age * age * 0.35;   // eaten away as it dies
        a = coverage(d, softness * 0.5);
        heat = clamp(-d / 0.55, 0.0, 1.0);
    } else if (sprite == SPRITE_SPARK) {
        // A thin capsule with a white-hot core; usually stretched along its velocity.
        float d = length(vec2(uv.x, max(abs(uv.y) - 0.55, 0.0))) - 0.42;
        a = coverage(d, softness * 0.5);
        heat = clamp(1.0 - length(vec2(uv.x * 1.6, uv.y * 0.7)), 0.0, 1.0);
    } else if (sprite == SPRITE_RING) {
        float wobble = (vnoise(vec2(atan(uv.y, uv.x) * 3.0, rnd * 5.0 + t)) - 0.5) * 0.08 * distort;
        float d = abs(r - 0.78 + wobble) - mix(0.14, 0.04, age);
        a = coverage(d, softness * 0.5);
    } else if (sprite == SPRITE_STAR) {
        // Four-point glint: two thin crossed rays over a soft core, twinkling with the seed.
        float twinkle = 0.75 + 0.25 * sin(t * 9.0 + rnd * 40.0);
        float rays = max(1.0 - abs(uv.x) * 9.0, 0.0) * (1.0 - abs(uv.y)) +
                     max(1.0 - abs(uv.y) * 9.0, 0.0) * (1.0 - abs(uv.x));
        float core = clamp(1.0 - r * 2.2, 0.0, 1.0);
        a = clamp(rays * twinkle + core * core, 0.0, 1.0);
        heat = core;
    } else if (sprite == SPRITE_LEAF) {
        // A pointed leaf with a midrib; lit as a flat two-sided card.
        float y = uv.y;
        float w = 0.62 * sqrt(max(0.0, 1.0 - y * y)) * (1.0 - 0.25 * y);
        float edge = (vnoise(vec2(y * 6.0, rnd * 9.0)) - 0.5) * 0.08 * distort;
        float d = abs(uv.x) - (w + edge);
        d = max(d, abs(y) - 0.98);
        a = coverage(d, softness * 0.4);
        float rib = smoothstep(0.07, 0.0, abs(uv.x)) * step(y, 0.85);
        albedo *= 1.0 - 0.28 * rib;
        albedo *= 0.88 + 0.24 * smoothstep(-1.0, 1.0, uv.x + uv.y * 0.3);   // soft fold
    } else {
        // SPRITE_TEXTURE: the albedo map, as a flipbook atlas when it has more than one cell.
        vec2 tiles = max(pc.mode.zw, vec2(1.0));
        float frame = v_misc.z;
        vec2 cell = vec2(mod(frame, tiles.x), floor(frame / tiles.x));
        vec2 tuv = (vec2(uv.x, -uv.y) * 0.5 + 0.5 + cell) / tiles;
        vec4 tex = texture(u_albedo_map, tuv);
        albedo *= tex.rgb;
        a = tex.a;
    }

    // --- colour: flame / spark cores burn toward white-yellow -----------------------------
    if (heat > 0.0) {
        float h = bands > 1.5 ? toon(heat, bands) : heat;
        float peak = max(max(albedo.r, albedo.g), albedo.b);
        vec3 hot = vec3(1.0, 0.92, 0.62) * max(peak, 0.5);
        albedo = mix(albedo, hot, h * h * 0.85) * (1.0 + h * 1.6);
    }

    // --- fades ------------------------------------------------------------------------------
    float view_depth = v_misc.w;
    float fade = 1.0;
    float scene_depth = 1e30;
    bool mask_pass = pc.extra.x > 0.5;
    if (pc.shape.y > 0.0 || mask_pass) {
        float scene_raw = texelFetch(u_hiz_map, ivec2(gl_FragCoord.xy), 0).r;
        scene_depth = gfx_linear_depth(scene_raw, pc.depth.x, pc.depth.y, pc.misc.w);
    }
    if (pc.shape.y > 0.0) fade *= clamp((scene_depth - view_depth) / pc.shape.y, 0.0, 1.0);
    if (pc.shape.z > 0.0) fade *= clamp((view_depth - pc.depth.x) / pc.shape.z, 0.0, 1.0);

    a *= v_color.a * pc.misc.x * fade;
    if (a < 0.002) discard;

    // The TAA reactive mask: coverage only, hidden behind opaque geometry by hand.
    if (mask_pass) {
        if (view_depth > scene_depth + 0.02) discard;
        out_color = vec4(clamp(a * pc.extra.y, 0.0, 1.0), 0.0, 0.0, 1.0);
        return;
    }

    // --- lighting --------------------------------------------------------------------------
    float lit = pc.shading.x;
    vec3 color = albedo;
    if (lit > 0.0) {
        if (sphere_normal) {
            // A ball's normal: the quad axes plus "toward the viewer" by how central the pixel is.
            float z = sqrt(max(1.0 - min(r * r, 1.0), 0.06));
            N = normalize(v_right * uv.x * 0.9 + v_up * uv.y * 0.9 + v_normal * z);
        }
        vec3 V = normalize(camera.camera_pos - v_world);
        bool shadows = pc.ambient.y > 0.5;
        float scatter_k = pc.extra.z;
        float g = clamp(pc.extra.w, -0.95, 0.95);
        // Henyey-Greenstein, scaled so isotropic scattering is 1: `cos_t` between the eye's view
        // ray (eye -> particle) and the light's direction toward the particle's far side.
        #define HG(cos_t) ((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * (cos_t), 1e-4), 1.5))
        vec3 light = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                   * pc.ambient.x;
        if (lights.light_counts.x > 0u) {
            vec3 L = normalize(-lights.dir_direction.xyz);
            // Half-Lambert: a volume is lit through, never pitch black on its shadow side.
            float wrap = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
            wrap = toon(wrap * wrap, bands);
            // The original soft forward lobe (smoke glows with the sun behind it), plus `scatter`.
            float glow = pow(clamp(dot(-V, L), 0.0, 1.0), 6.0) * 0.6 + scatter_k * HG(dot(-V, L)) * 0.25;
            float vis = 1.0;
            if (shadows && lights.dir_shadow_params.z > 0.5) vis = 1.0 - particle_dir_shadow(v_world);
            light += lights.dir_color.rgb * lights.dir_direction.w * (wrap + glow) * vis;
        }
        uint num_points = min(lights.light_counts.y, 16u);
        for (uint i = 0u; i < num_points; ++i) {
            PointLight pl = lights.point_lights[i];
            vec3 to_l = pl.position_range.xyz - v_world;
            float dist = length(to_l);
            float range = pl.position_range.w;
            if (dist > range || dist < 1e-4) continue;
            vec3 L = to_l / dist;
            // Same distance curve as the forward / deferred point lights (pixel_forward_shading.glsl).
            float sharpness = max(pl.attenuation.x, 0.1);
            float factor = clamp(dist / range, 0.0, 1.0);
            float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
            falloff *= falloff;
            float atten = falloff / (4.0 * 3.14159265 * (factor * factor + 1.0));
            float wrap = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
            float vis = (shadows && pl.attenuation.w > 0.5) ? 1.0 - calc_local_shadow(pl.attenuation.w, v_world, L, L) : 1.0;
            light += pl.color_intensity.rgb * (pl.color_intensity.w * 0.08) * atten * vis *
                     (toon(wrap, bands) + scatter_k * HG(dot(-V, L)));
        }
        uint num_spots = min(lights.light_counts.z, 8u);
        for (uint i = 0u; i < num_spots; ++i) {
            SpotLight sl = lights.spot_lights[i];
            vec3 to_l = sl.position_range.xyz - v_world;
            float dist = length(to_l);
            float range = sl.position_range.w;
            if (dist > range || dist < 1e-4) continue;
            vec3 L = to_l / dist;
            float cone = gfx_spot_cone(L, sl.direction_cone.xyz, sl.direction_cone.w, sl.params.y);
            if (cone <= 0.0) continue;
            float sharpness = max(sl.params.x, 0.1);
            float factor = clamp(dist / range, 0.0, 1.0);
            float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
            falloff *= falloff;
            float atten = falloff / (4.0 * 3.14159265 * (factor * factor + 1.0)) * cone;
            float wrap = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
            float vis = (shadows && sl.params.z > 0.5) ? 1.0 - calc_local_shadow(sl.params.z, v_world, L, L) : 1.0;
            light += sl.color_intensity.rgb * (sl.color_intensity.w * 0.08) * atten * vis *
                     (toon(wrap, bands) + scatter_k * HG(dot(-V, L)));
        }
        #undef HG
        color = mix(albedo, albedo * light, lit);
    }
    color *= pc.shading.z;   // emissive / HDR multiplier

    out_color = vec4(color * a, a * (1.0 - pc.shading.w));
}
