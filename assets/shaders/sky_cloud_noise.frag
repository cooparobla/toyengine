#version 450

// The cloud layer's 2D shape map (512 x 512, tiling), rendered once by SkyCloudPass:
//   r = the cloud field: Perlin fbm carved by inverted Worley cells (billowy, Schneider's
//       "Perlin-Worley"), the layer's coverage threshold cuts it
//   g = finer inverted-Worley fbm (spare: the march erodes with 3D noise instead -- a 2D detail
//       read extrudes into vertical streaks)
//   b = very low-frequency fbm, varying coverage and height across the sky
// Every octave's lattice wraps at the texture edge, so the map tiles seamlessly (the cloud
// march samples it with a repeating sampler).

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Gradient noise on a lattice wrapping every `period` cells, in [-1, 1]-ish.
float tile_perlin(vec2 p, float period) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 g00 = hash22(mod(i, period)) * 2.0 - 1.0;
    vec2 g10 = hash22(mod(i + vec2(1, 0), period)) * 2.0 - 1.0;
    vec2 g01 = hash22(mod(i + vec2(0, 1), period)) * 2.0 - 1.0;
    vec2 g11 = hash22(mod(i + vec2(1, 1), period)) * 2.0 - 1.0;
    float n00 = dot(g00, f), n10 = dot(g10, f - vec2(1, 0));
    float n01 = dot(g01, f - vec2(0, 1)), n11 = dot(g11, f - vec2(1, 1));
    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y) * 1.4;
}

// Worley F1 distance, cells wrapping every `period`, in [0, ~1].
float tile_worley(vec2 p, float period) {
    vec2 i = floor(p), f = fract(p);
    float d = 1e9;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(x, y);
            vec2 pt = hash22(mod(i + o, period));
            vec2 r = o + pt - f;
            d = min(d, dot(r, r));
        }
    }
    return sqrt(d);
}

float perlin_fbm(vec2 uv, float period, int octaves) {
    float s = 0.0, a = 0.5, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        s += a * tile_perlin(uv * period, period);
        norm += a;
        period *= 2.0;
        a *= 0.5;
    }
    return s / norm;
}

float worley_fbm(vec2 uv, float period) {
    return 0.625 * (1.0 - tile_worley(uv * period, period))
         + 0.25 * (1.0 - tile_worley(uv * period * 2.0, period * 2.0))
         + 0.125 * (1.0 - tile_worley(uv * period * 4.0, period * 4.0));
}

float remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

void main() {
    vec2 uv = in_uv;
    // Low octaves only: the march extrudes this map vertically, so any fine 2D detail would
    // show as vertical streaks. Small-scale detail comes from the march's 3D noise instead.
    float perlin = clamp(perlin_fbm(uv, 6.0, 3) * 0.5 + 0.5, 0.0, 1.0);
    float worley = (0.625 * (1.0 - tile_worley(uv * 8.0, 8.0)) + 0.25 * (1.0 - tile_worley(uv * 16.0, 16.0))) / 0.875;
    // Perlin-Worley: the Perlin field, pushed up inside Worley cells -- rounded, billowing
    // blobs instead of fbm's stringy wisps.
    float field = clamp(remap(perlin, worley - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
    // The raw field sits in a narrow band (5th..95th percentile ~0.54..0.74): stretch it to an
    // even spread over [0, 1], so a coverage of c covers about c of the sky.
    field = smoothstep(0.53, 0.75, field);
    float detail = worley_fbm(uv, 32.0);
    float variation = clamp(perlin_fbm(uv, 2.0, 3) * 0.5 + 0.5, 0.0, 1.0);
    out_color = vec4(field, detail, variation, 1.0);
}
