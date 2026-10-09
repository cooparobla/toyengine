#ifndef TOY_SKY_PHYSICAL_GLSL
#define TOY_SKY_PHYSICAL_GLSL

// sky_physical.glsl -- the physical sky as the lighting pass draws it at background pixels
// (render sky_model: physical): the sky-view LUT, the stars, the sun's and the moon's discs, and
// the half-resolution cloud layer upsampled over them.
//
// REQUIRED BEFORE INCLUDE: the `camera` block (camera_pos), the `lights` block
// (light_ubo_body.glsl: sky_sun / sky_moon / sky_params / sky_extra), and the samplers
// u_sky_transmittance, u_sky_view and u_sky_clouds.

#include "sky_atmosphere.glsl"

float sky_hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
vec3 sky_hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Smooth blotches over the moon's face (its darker maria), in [0, 1].
float sky_moon_maria(vec2 p) {
    float v = 0.0, a = 0.5;
    p = p * 2.2 + 7.0;
    for (int o = 0; o < 4; ++o) {
        vec2 i = floor(p), f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float n = mix(mix(sky_hash13(vec3(i, 1.0)), sky_hash13(vec3(i + vec2(1, 0), 1.0)), f.x),
                      mix(sky_hash13(vec3(i + vec2(0, 1), 1.0)), sky_hash13(vec3(i + vec2(1, 1), 1.0)), f.x), f.y);
        v += a * n;
        p *= 2.1;
        a *= 0.5;
    }
    return smoothstep(0.45, 0.75, v);
}

// One layer of stars: at most one per cell of a grid over the direction cube, each a tiny
// gaussian point with its own brightness and colour temperature.
vec3 sky_star_layer(vec3 dir, float cells, float density, float t) {
    vec3 c = floor(dir * cells);
    vec3 col = vec3(0.0);
    float h = sky_hash13(c + 17.0);
    if (h > density) return col;
    vec3 j = sky_hash33(c);
    vec3 star = normalize(c + 0.3 + 0.4 * j);   // kept off the cell's edges, which would cut it
    float ang = acos(clamp(dot(dir, star), -1.0, 1.0));
    float size = 0.00075;   // radians: about a pixel at 1080p and a 60-degree field of view
    float b = exp(-(ang * ang) / (size * size));
    float mag = pow(sky_hash13(c + 3.1), 6.0);
    float twinkle = 0.75 + 0.25 * sin(t * (2.0 + 5.0 * j.x) + 40.0 * j.y);
    vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.85, 0.7), j.z);
    return tint * b * (0.15 + 3.0 * mag) * twinkle;
}

// The cloud layer's colour (rgb) and transmittance (a) at a full-resolution uv: a bilinear
// upsample of the reconstructed layer (sky_cloud_resolve.frag) that drops taps skipped as
// geometry (a < 0). The layer fills
// the top-left `scale` fraction of its half-resolution target (sky_quality; the same rounding
// as SkyCloudPass::scaled_extent).
vec4 sky_clouds_upsample(vec2 uv, float scale) {
    ivec2 size = clamp(ivec2(floor(vec2(textureSize(u_sky_clouds, 0)) * scale + 0.5)), ivec2(1), textureSize(u_sky_clouds, 0));
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y <= 1; ++y) {
        for (int x = 0; x <= 1; ++x) {
            ivec2 q = clamp(i + ivec2(x, y), ivec2(0), size - 1);
            vec4 s = texelFetch(u_sky_clouds, q, 0);
            float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            if (s.a < 0.0) continue;
            sum += s * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : vec4(0.0, 0.0, 0.0, 1.0);
}

vec3 sky_physical(vec3 dir, vec2 uv) {
    SkyAtmosphere atmo;
    atmo.ozone.w = 6360.0;
    atmo.ground.w = 6460.0;
    float view_r = sky_view_radius(atmo, camera.camera_pos.z);
    vec3 sun = lights.sky_sun.xyz;
    vec3 sky = sky_view_lookup(u_sky_view, atmo, view_r, dir, sun) * lights.sky_params.w;
    // Night airglow / light pollution floor, so a moonless night is not pure black.
    sky += vec3(0.55, 0.75, 1.3) * 1e-3 * lights.sky_extra.z;

    bool above = sky_ray_sphere(vec3(0.0, 0.0, view_r), dir, atmo.ozone.w) < 0.0;
    if (above) {
        vec3 t_view = sky_transmittance(u_sky_transmittance, atmo, view_r, dir.z);
        float star_vis = lights.sky_params.z;
        if (star_vis > 0.0) {
            vec3 stars = sky_star_layer(dir, 260.0, 0.22, lights.sky_extra.w)
                       + sky_star_layer(dir, 90.0, 0.25, lights.sky_extra.w * 0.7) * 2.5;
            sky += stars * star_vis * t_view * 0.03;
        }
        // Sun: a limb-darkened disc with an anti-aliased rim.
        float cs = dot(dir, sun);
        float cos_r = lights.sky_sun.w;
        if (cs > cos_r - (1.0 - cos_r)) {
            float r = acos(clamp(cs, -1.0, 1.0)) / max(acos(clamp(cos_r, -1.0, 1.0)), 1e-5);   // 0 centre .. 1 rim
            float edge = 1.0 - smoothstep(0.9, 1.1, r);
            float mu = sqrt(max(1.0 - min(r, 1.0) * min(r, 1.0), 0.0));
            float limb = 1.0 - 0.6 * (1.0 - pow(mu, 0.8));
            sky += t_view * lights.sky_extra.x * edge * limb;
        }
        // Moon: a full disc (it is always opposite the sun) with darker maria.
        vec3 moon = lights.sky_moon.xyz;
        float cm = dot(dir, moon);
        float cos_m = lights.sky_moon.w;
        if (lights.sky_extra.y > 0.0 && cm > cos_m - (1.0 - cos_m)) {
            float rad = max(acos(clamp(cos_m, -1.0, 1.0)), 1e-5);
            float r = acos(clamp(cm, -1.0, 1.0)) / rad;
            float edge = 1.0 - smoothstep(0.92, 1.08, r);
            vec3 side = normalize(cross(moon, abs(moon.z) < 0.99 ? vec3(0, 0, 1) : vec3(1, 0, 0)));
            vec3 upv = cross(side, moon);
            vec2 local = vec2(dot(dir - moon, side), dot(dir - moon, upv)) / rad;
            float maria = sky_moon_maria(local);
            sky += t_view * lights.sky_extra.y * edge * mix(1.0, 0.62, maria) * vec3(0.95, 0.96, 1.0);
        }
    }
    if (lights.sky_params.y > 0.0) {
        vec4 c = sky_clouds_upsample(uv, lights.sky_params.y);
        sky = sky * c.a + c.rgb;
    }
    return sky;
}

#endif // TOY_SKY_PHYSICAL_GLSL
