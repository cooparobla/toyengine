// Tiling noise for the cloud textures' compute generators (sky_cloud_weather.comp,
// sky_cloud_shape.comp, sky_cloud_detail.comp). Every lattice wraps every `period` cells, so the
// baked textures repeat seamlessly under a repeating sampler. Coordinates are in cells.

uvec3 cn_pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// Uniform [0, 1)^3 for an integer lattice point, wrapped to the period first.
vec3 cn_hash(ivec3 c, int period, uint seed) {
    ivec3 w = ((c % period) + period) % period;
    return vec3(cn_pcg3d(uvec3(w) + uvec3(seed, seed * 7u, seed * 13u))) * (1.0 / 4294967296.0);
}

// Gradient (Perlin) noise, roughly [-1, 1].
float cn_perlin(vec3 p, int period, uint seed) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n[8];
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        vec3 g = normalize(cn_hash(i + o, period, seed) * 2.0 - 1.0 + 1e-4);
        n[k] = dot(g, f - vec3(o));
    }
    return mix(mix(mix(n[0], n[1], u.x), mix(n[2], n[3], u.x), u.y),
               mix(mix(n[4], n[5], u.x), mix(n[6], n[7], u.x), u.y), u.z) * 1.2;
}

// Worley F1 distance, [0, ~1].
float cn_worley(vec3 p, int period, uint seed) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    float d = 1e9;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        ivec3 o = ivec3(x, y, z);
        vec3 r = vec3(o) + cn_hash(i + o, period, seed) - f;
        d = min(d, dot(r, r));
    }
    return sqrt(d);
}

// Perlin fbm over `octaves`, in [0, 1]. uvw spans one period of the texture in [0, 1).
float cn_perlin_fbm(vec3 uvw, int freq, int octaves, uint seed) {
    float s = 0.0, a = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        s += a * cn_perlin(uvw * float(freq), freq, seed + uint(o) * 31u);
        norm += a;
        freq *= 2;
        a *= 0.5;
    }
    // fbm's spread is narrow (5th..95th percentile ~ -0.25..0.25): stretch it to ~[0.1, 0.9].
    return clamp(s / norm * 1.6 + 0.5, 0.0, 1.0);
}

// Inverted Worley fbm (three octaves): 1 in cell centres -- billowy cauliflower lumps,
// stretched from its natural spread (5th..95th percentile ~0.29..0.68) to [0, 1].
float cn_worley_fbm(vec3 uvw, int freq, uint seed) {
    float w = 0.625 * (1.0 - cn_worley(uvw * float(freq), freq, seed))
            + 0.25  * (1.0 - cn_worley(uvw * float(freq * 2), freq * 2, seed + 101u))
            + 0.125 * (1.0 - cn_worley(uvw * float(freq * 4), freq * 4, seed + 211u));
    return clamp((w - 0.25) / 0.48, 0.0, 1.0);
}

float cn_remap(float v, float lo, float hi, float nlo, float nhi) {
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}
