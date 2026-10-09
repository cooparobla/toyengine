// Shared by the GPU particle compute passes (particles_init / _simulate / _emit / _sort /
// _gather.comp) -- see toyengine/render/passes/gpu_particle_pass.h. Every binding is one
// system's: set 0 is allocated per system and frame slot.
//
// The maths mirrors toyengine/particles/particle_system.h (update_(), spawn_(), prepare_render())
// and particle_shape.h, so a `simulation: gpu` system looks like its CPU twin. Only the random
// streams differ (per-particle PCG hashes instead of the emitter's serial Rng).

// toy::render::GpuParticleParams (particle_types.h) -- std430, vec4 / mat4 only.
layout(std430, set = 0, binding = 0) readonly buffer ParamsBuf {
    mat4 world;
    mat4 prev_world;
    mat4 to_world;
    vec4 frame;        // x dt, y noise time, z spawn count, w spawn seed (uint bits)
    vec4 config;       // x capacity, y sort mode, z local, w world scale
    vec4 eye;          // xyz camera, w flipbook frames
    vec4 shape0;       // x type, y radius, z thickness, w cone angle (deg)
    vec4 shape1;       // x arc (deg), y random direction, z normal offset, w edge length
    vec4 shape_box;    // xyz box, w triangle count
    vec4 shape_offset; // xyz offset, w total area
    vec4 life_speed;
    vec4 size_rot;
    vec4 spin;         // xy angular velocity (rad/s), z align, w random spin
    vec4 color_a;
    vec4 color_b;
    vec4 accel;        // xyz gravity + force, w drag
    vec4 velocity;     // xyz constant, w orbital
    vec4 center;       // xyz orbit centre, w radial
    vec4 axis;         // xyz orbit axis, w tumble (rad/s)
    vec4 collision;    // x collide, y ground, z bounce, w horizontal keep
    vec4 wrap;         // xyz wrap box, w fade fraction
    vec4 noise;        // x strength, y kill on collide, z flipbook mode, w flipbook fps
    vec4 misc;         // x flipbook cycles, yzw inherited emitter velocity
    vec4 emitter_rot;  // quaternion xyzw
    vec4 noise_k[6];
    vec4 noise_c[6];
    vec4 color_lut[64];
    vec4 curve_lut[64];
} P;

// One particle: pos_age (xyz simulation-space position, w age), vel_life (xyz velocity, w life;
// <= 0 = a free slot), color0, orient (quaternion), misc (x size0, y rotation, z spin), seed.
struct Particle {
    vec4 pos_age;
    vec4 vel_life;
    vec4 color0;
    vec4 orient;
    vec3 misc;
    uint seed;
};

// toy::render::ParticleInstance -- what particle.vert reads at instance rate.
struct Instance {
    vec4 pos_size;
    vec4 color;
    vec4 velocity_rot;
    vec4 orient;
    vec4 misc;
};

layout(std430, set = 0, binding = 1) buffer ParticlesBuf { Particle particles[]; };
// Header = a VkDrawIndirectCommand (6 vertices, instance_count = the alive list's atomic
// counter) + the free list's count; then the free list itself.
layout(std430, set = 0, binding = 2) buffer ListsBuf {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
    int  free_count;
    uint pad0;
    uint pad1;
    uint pad2;
    uint free_list[];
};
layout(std430, set = 0, binding = 3) buffer OutBuf { Instance out_inst[]; };       // drawn
layout(std430, set = 0, binding = 4) buffer UnsortedBuf { Instance unsorted_inst[]; }; // sort input
layout(std430, set = 0, binding = 5) buffer KeysBuf { uvec2 keys[]; };            // (key, index)
layout(std430, set = 0, binding = 6) readonly buffer TrisBuf { vec4 tris[]; };    // 8 vec4 / triangle

layout(push_constant) uniform GpuParticlePush {
    uint mode;
    uint k;
    uint j;
    uint pad;
} pc;

const float PI2 = 6.28318530718;

// --- hashing / RNG -----------------------------------------------------------------------------

// lowbias32: toy::particles::hash_u32 (the CPU's per-particle hash, so tumble axes and flipbook
// randoms are the same function of a particle's seed).
uint hash_u32(uint x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float hash01(uint x) { return float(hash_u32(x) >> 8) * (1.0 / 16777216.0); }

uint rng_u32(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rng01(inout uint s) { return float(rng_u32(s) >> 8) * (1.0 / 16777216.0); }
float rng_range(inout uint s, vec2 r) { return r.x == r.y ? r.x : r.x + (r.y - r.x) * rng01(s); }
float rng_signed(inout uint s) { return rng01(s) * 2.0 - 1.0; }
vec3 rng_unit(inout uint s) {
    float z = rng_signed(s);
    float a = rng01(s) * PI2;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}

// --- quaternions (xyzw, glm's Hamilton product) -------------------------------------------------

vec4 quat_mul(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}
vec4 quat_axis_angle(vec3 axis, float angle) {
    return vec4(axis * sin(0.5 * angle), cos(0.5 * angle));
}
vec4 quat_from_mat3(mat3 m) {
    float tr = m[0][0] + m[1][1] + m[2][2];
    vec4 q;
    if (tr > 0.0) {
        float s = sqrt(tr + 1.0) * 2.0;
        q = vec4((m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s, 0.25 * s);
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q = vec4(0.25 * s, (m[1][0] + m[0][1]) / s, (m[2][0] + m[0][2]) / s, (m[1][2] - m[2][1]) / s);
    } else if (m[1][1] > m[2][2]) {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q = vec4((m[1][0] + m[0][1]) / s, 0.25 * s, (m[2][1] + m[1][2]) / s, (m[2][0] - m[0][2]) / s);
    } else {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q = vec4((m[2][0] + m[0][2]) / s, (m[2][1] + m[1][2]) / s, 0.25 * s, (m[0][1] - m[1][0]) / s);
    }
    return normalize(q);
}
// toy::particles::quat_from_normal_tangent: +Z = n, +X as close to `tangent` as possible.
vec4 quat_from_normal_tangent(vec3 n, vec3 tangent) {
    vec3 t = tangent - n * dot(n, tangent);
    float len = length(t);
    if (len < 1e-5) t = abs(n.z) < 0.9 ? normalize(cross(vec3(0.0, 0.0, 1.0), n)) : normalize(cross(vec3(1.0, 0.0, 0.0), n));
    else t /= len;
    return quat_from_mat3(mat3(t, cross(n, t), n));
}

// --- curves, turbulence --------------------------------------------------------------------------

vec4 lut_color(float t) {
    float x = clamp(t, 0.0, 1.0) * 63.0;
    int i = min(int(x), 62);
    return mix(P.color_lut[i], P.color_lut[i + 1], x - float(i));
}
vec4 lut_curve(float t) {
    float x = clamp(t, 0.0, 1.0) * 63.0;
    int i = min(int(x), 62);
    return mix(P.curve_lut[i], P.curve_lut[i + 1], x - float(i));
}

// toy::particles::TurbulenceField::sample -- the same six curl-of-plane-wave terms.
vec3 turbulence(vec3 p, float t) {
    vec3 v = vec3(0.0);
    for (int i = 0; i < 6; ++i) {
        v += P.noise_c[i].xyz * cos(dot(P.noise_k[i].xyz, p) + P.noise_k[i].w * t + P.noise_c[i].w);
    }
    return v;
}

// --- the alive list ------------------------------------------------------------------------------

uint order_key(float f) {
    uint u = floatBitsToUint(f);
    return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
}

// prepare_render() for one particle, appended to this frame's alive list (the draw's instances).
void append_instance(Particle p) {
    bool local = P.config.z > 0.5;
    float life = p.vel_life.w;
    float age = p.pos_age.w;
    float t = life > 0.0 ? clamp(age / life, 0.0, 1.0) : 1.0;
    vec4 curve = lut_curve(t);
    Instance inst;
    float size = p.misc.x * curve.x * P.config.w;
    vec4 color = p.color0 * lut_color(t);
    color.a *= curve.y;
    vec3 wp = (P.to_world * vec4(p.pos_age.xyz, 1.0)).xyz;
    vec2 half_wrap = local ? vec2(0.0) : P.wrap.xy * 0.5;
    if (P.wrap.w > 0.0 && (half_wrap.x > 0.0 || half_wrap.y > 0.0)) {
        float fade_frac = clamp(P.wrap.w, 1e-3, 1.0);
        vec2 d = abs(wp.xy - P.world[3].xy);
        float f = 1.0;
        for (int a = 0; a < 2; ++a) {
            if (half_wrap[a] <= 0.0) continue;
            f *= clamp((half_wrap[a] - d[a]) / (half_wrap[a] * fade_frac), 0.0, 1.0);
        }
        color.a *= f;
    }
    uint seed = p.seed;
    vec4 q = local ? quat_mul(P.emitter_rot, p.orient) : p.orient;
    if (P.axis.w != 0.0) {
        vec3 ax = normalize(vec3(hash01(seed * 3u + 11u) - 0.5, hash01(seed * 3u + 12u) - 0.5,
                                 hash01(seed * 3u + 13u) - 0.5) + vec3(0.0, 0.0, 1e-4));
        q = quat_mul(q, quat_axis_angle(ax, P.axis.w * age));
    }
    inst.pos_size = vec4(wp, size);
    inst.color = color;
    inst.velocity_rot = vec4(mat3(P.to_world) * (p.vel_life.xyz + P.velocity.xyz), p.misc.y);
    inst.orient = q;
    float rnd = hash01(seed);
    float frames = P.eye.w;
    float fr = 0.0;
    if (frames > 1.0) {
        int fm = int(P.noise.z + 0.5);
        if (fm == 1)      fr = floor(rnd * frames);
        else if (fm == 2) fr = mod(floor(age * P.noise.w + rnd * frames), frames);
        else              fr = mod(floor(t * frames * max(P.misc.x, 1e-3)), frames);
    }
    inst.misc = vec4(t, rnd, fr, 0.0);

    uint idx = atomicAdd(instance_count, 1u);
    int sort_mode = int(P.config.y + 0.5);
    if (sort_mode == 0) {
        out_inst[idx] = inst;
    } else {
        float key = 0.0;
        if (sort_mode == 1) { vec3 d = wp - P.eye.xyz; key = -dot(d, d); }   // farthest first
        else if (sort_mode == 2) key = -age;                                   // oldest first
        else key = age;                                                        // youngest first
        unsorted_inst[idx] = inst;
        keys[idx] = uvec2(order_key(key), idx);
    }
}
