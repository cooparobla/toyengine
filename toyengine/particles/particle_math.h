/**
 * @file particle_math.h
 * @brief The small, Vulkan-free building blocks every particle module samples: a seeded RNG,
 *        min/max ranges, float curves and colour gradients (both baked to lookup tables), and a
 *        divergence-free turbulence field.
 *
 * Everything here is plain data plus pure functions, so the simulation that uses it stays
 * unit-testable with no device, and is safe to evaluate from any number of job workers at once
 * (nothing is lazily mutated on read: curves and gradients bake in bake(), called once when the
 * settings are applied).
 */

#ifndef TOYENGINE_PARTICLES_PARTICLE_MATH_H
#define TOYENGINE_PARTICLES_PARTICLE_MATH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace toy {
namespace particles {

/**
 * @brief Small, fast, seedable RNG (PCG32, O'Neill). One per emitter, plus a per-particle
 *        32-bit seed for anything evaluated later in the particle's life, so the result never
 *        depends on how the work was split across threads.
 */
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc   = 0xda3e39cb94b95bdbULL;

    Rng() = default;
    explicit Rng(uint64_t seed, uint64_t stream = 1) { reseed(seed, stream); }

    void reseed(uint64_t seed, uint64_t stream = 1) {
        state = 0u;
        inc = (stream << 1u) | 1u;
        next_u32();
        state += seed;
        next_u32();
    }

    uint32_t next_u32() {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    /** @brief Uniform in [0, 1). */
    float next01() { return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f); }
    /** @brief Uniform in [lo, hi). */
    float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
    /** @brief Uniform in [-1, 1). */
    float signed01() { return next01() * 2.0f - 1.0f; }

    /** @brief Uniform direction on the unit sphere. */
    glm::vec3 unit_vector() {
        const float z = signed01();
        const float a = next01() * 6.28318530718f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return {r * std::cos(a), r * std::sin(a), z};
    }
};

/** @brief Stateless integer hash (lowbias32) -> [0, 1). For per-particle values derived from its seed. */
inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float hash01(uint32_t x) { return static_cast<float>(hash_u32(x) >> 8) * (1.0f / 16777216.0f); }

/**
 * @struct Range
 * @brief Unity's "Random Between Two Constants": a value drawn uniformly from [min, max] per
 *        particle. min == max is a constant.
 */
struct Range {
    float min = 0.0f;
    float max = 0.0f;

    Range() = default;
    Range(float v) : min(v), max(v) {}                 // NOLINT(google-explicit-constructor): `Range r = 2.0f` reads as a constant
    Range(float lo, float hi) : min(lo), max(hi) {}

    float sample(Rng& rng) const { return min == max ? min : rng.range(min, max); }
    float lerp(float t) const { return min + (max - min) * t; }
    float mid() const { return 0.5f * (min + max); }
    float largest() const { return std::max(min, max); }
};

/**
 * @class FloatCurve
 * @brief A piecewise-linear curve over normalized time [0, 1] (Unity's AnimationCurve, Blender's
 *        curve mapping), baked to a lookup table so evaluating it per particle per frame is one
 *        lerp. An empty curve evaluates to `fallback` everywhere.
 */
class FloatCurve {
public:
    static constexpr int kSamples = 64;

    FloatCurve() = default;
    explicit FloatCurve(std::vector<glm::vec2> keys, float fallback = 1.0f) : keys_(std::move(keys)), fallback_(fallback) { bake(); }

    /** @brief Two-key ramp from `a` at t=0 to `b` at t=1. */
    static FloatCurve linear(float a, float b) { return FloatCurve({{0.0f, a}, {1.0f, b}}); }

    void set_keys(std::vector<glm::vec2> keys) { keys_ = std::move(keys); bake(); }
    const std::vector<glm::vec2>& keys() const { return keys_; }
    bool empty() const { return keys_.empty(); }
    void set_fallback(float v) { fallback_ = v; bake(); }

    /** @brief Exact piecewise-linear evaluation over the keys (sorted by t), clamped at the ends. */
    float evaluate_exact(float t) const {
        if (keys_.empty()) return fallback_;
        if (t <= keys_.front().x) return keys_.front().y;
        if (t >= keys_.back().x) return keys_.back().y;
        for (size_t i = 1; i < keys_.size(); ++i) {
            if (t <= keys_[i].x) {
                const glm::vec2& a = keys_[i - 1];
                const glm::vec2& b = keys_[i];
                const float span = b.x - a.x;
                return span <= 1e-6f ? b.y : a.y + (b.y - a.y) * ((t - a.x) / span);
            }
        }
        return keys_.back().y;
    }

    /** @brief Table lookup; `t` is clamped to [0, 1]. */
    float evaluate(float t) const {
        const float x = std::clamp(t, 0.0f, 1.0f) * (kSamples - 1);
        const int i = std::min(static_cast<int>(x), kSamples - 2);
        const float f = x - static_cast<float>(i);
        return lut_[i] + (lut_[i + 1] - lut_[i]) * f;
    }

    /** @brief Integral of the curve over [0, t] (trapezoid over the table). For distance-over-life. */
    float integral(float t) const {
        const float x = std::clamp(t, 0.0f, 1.0f) * (kSamples - 1);
        const int n = static_cast<int>(x);
        const float step = 1.0f / (kSamples - 1);
        float sum = 0.0f;
        for (int i = 0; i < n && i < kSamples - 1; ++i) sum += 0.5f * (lut_[i] + lut_[i + 1]) * step;
        if (n < kSamples - 1) {
            const float f = x - static_cast<float>(n);
            const float end = lut_[n] + (lut_[n + 1] - lut_[n]) * f;
            sum += 0.5f * (lut_[n] + end) * f * step;
        }
        return sum;
    }

    /** @brief Re-sorts the keys and rebuilds the table. Called by every mutator. */
    void bake() {
        std::sort(keys_.begin(), keys_.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
        for (int i = 0; i < kSamples; ++i) lut_[i] = evaluate_exact(static_cast<float>(i) / (kSamples - 1));
    }

private:
    std::vector<glm::vec2> keys_;
    float fallback_ = 1.0f;
    std::array<float, kSamples> lut_ = filled_(1.0f);

    static std::array<float, kSamples> filled_(float v) { std::array<float, kSamples> a{}; a.fill(v); return a; }
};

/**
 * @class Gradient
 * @brief An RGBA gradient over normalized time (Unity's Gradient, Blender's colour ramp), baked
 *        to a table like FloatCurve. Colours are linear. An empty gradient is white, opaque.
 */
class Gradient {
public:
    static constexpr int kSamples = 64;

    struct Key {
        float     t = 0.0f;
        glm::vec4 color{1.0f};
    };

    Gradient() { lut_.fill(glm::vec4(1.0f)); }
    explicit Gradient(std::vector<Key> keys) : keys_(std::move(keys)) { bake(); }

    void set_keys(std::vector<Key> keys) { keys_ = std::move(keys); bake(); }
    const std::vector<Key>& keys() const { return keys_; }
    bool empty() const { return keys_.empty(); }

    glm::vec4 evaluate_exact(float t) const {
        if (keys_.empty()) return glm::vec4(1.0f);
        if (t <= keys_.front().t) return keys_.front().color;
        if (t >= keys_.back().t) return keys_.back().color;
        for (size_t i = 1; i < keys_.size(); ++i) {
            if (t <= keys_[i].t) {
                const Key& a = keys_[i - 1];
                const Key& b = keys_[i];
                const float span = b.t - a.t;
                return span <= 1e-6f ? b.color : glm::mix(a.color, b.color, (t - a.t) / span);
            }
        }
        return keys_.back().color;
    }

    glm::vec4 evaluate(float t) const {
        const float x = std::clamp(t, 0.0f, 1.0f) * (kSamples - 1);
        const int i = std::min(static_cast<int>(x), kSamples - 2);
        return glm::mix(lut_[i], lut_[i + 1], x - static_cast<float>(i));
    }

    void bake() {
        std::sort(keys_.begin(), keys_.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
        for (int i = 0; i < kSamples; ++i) lut_[i] = evaluate_exact(static_cast<float>(i) / (kSamples - 1));
    }

private:
    std::vector<Key> keys_;
    std::array<glm::vec4, kSamples> lut_{};
};

/**
 * @class TurbulenceField
 * @brief A smooth, divergence-free, time-varying velocity field (Unity's Noise module, Blender's
 *        Turbulence force field) -- the curl of a sum of plane-wave potentials.
 *
 * Curl rather than a plain noise vector because a divergence-free field swirls instead of
 * piling particles into sinks and sources: smoke curls and eddies rather than clumping. Plane
 * waves rather than gradient noise because the curl of `a * sin(k.p + w t)` is analytic -- six
 * waves cost six sin/cos pairs per particle, with no finite differencing.
 */
class TurbulenceField {
public:
    static constexpr int kWaves = 6;

    /** @brief Builds wave vectors at `frequency` (cycles / metre) from `seed`. Octaves double
     *         frequency and halve amplitude. */
    void configure(uint32_t seed, float frequency, int octaves = 2) {
        Rng rng(seed ^ 0x9e3779b9u, 7);
        octaves = std::clamp(octaves, 1, 3);
        float power = 0.0f;
        for (int i = 0; i < kWaves; ++i) {
            const int octave = i % octaves;
            const float f = frequency * static_cast<float>(1 << octave) * 6.28318530718f;
            Wave& w = waves_[i];
            w.k = rng.unit_vector() * f * rng.range(0.75f, 1.25f);
            w.a = rng.unit_vector() / static_cast<float>(1 << octave);
            w.omega = rng.range(0.6f, 1.4f) * (octave + 1);
            w.phase = rng.range(0.0f, 6.28318530718f);
            // Each wave adds cos(...) (k x a): a random-phase cosine has mean square 1/2.
            const float m = glm::length(glm::cross(w.k, w.a));
            power += 0.5f * m * m;
        }
        // Normalize to unit RMS magnitude, so `strength` is the field's typical value at any
        // frequency or octave count.
        norm_ = power > 0.0f ? 1.0f / std::sqrt(power) : 0.0f;
    }

    /**
     * @brief The field at `p`, time `t`: curl(sum a_i sin(k_i.p + omega_i t + phase_i)) =
     *        sum cos(...) (k_i x a_i), scaled to unit-ish magnitude.
     */
    glm::vec3 sample(const glm::vec3& p, float t) const {
        glm::vec3 v(0.0f);
        for (const Wave& w : waves_) {
            const float c = std::cos(glm::dot(w.k, p) + w.omega * t + w.phase);
            v += glm::cross(w.k, w.a) * c;
        }
        return v * norm_;
    }

private:
    struct Wave {
        glm::vec3 k{0.0f};
        glm::vec3 a{0.0f};
        float omega = 1.0f;
        float phase = 0.0f;
    };
    std::array<Wave, kWaves> waves_{};
    float norm_ = 0.0f;
};

/** @brief Rotation taking +Z to `n` (unit), with a stable choice of twist. */
inline glm::quat quat_from_normal(const glm::vec3& n) {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const float d = glm::dot(up, n);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) return glm::quat(0.0f, 1.0f, 0.0f, 0.0f);   // 180 deg about X
    const glm::vec3 axis = glm::cross(up, n);
    const float s = std::sqrt((1.0f + d) * 2.0f);
    return glm::normalize(glm::quat(s * 0.5f, axis.x / s, axis.y / s, axis.z / s));
}

/** @brief Orthonormal frame with +Z = `n` and +X as close to `tangent` as possible. */
inline glm::quat quat_from_normal_tangent(const glm::vec3& n, const glm::vec3& tangent) {
    glm::vec3 t = tangent - n * glm::dot(n, tangent);
    const float len = glm::length(t);
    if (len < 1e-5f) return quat_from_normal(n);
    t /= len;
    const glm::vec3 b = glm::cross(n, t);
    return glm::normalize(glm::quat_cast(glm::mat3(t, b, n)));
}

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_PARTICLE_MATH_H
