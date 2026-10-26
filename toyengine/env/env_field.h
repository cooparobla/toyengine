/**
 * @file env_field.h
 * @brief `EnvField` -- the whole environment at an instant: the scene-global weather term
 *        plus every local influence, sampled positionally.
 *
 * This is the one thing everything that reacts to the weather reads. `EnvSystem` (order 45)
 * gathers it once a frame and publishes an immutable snapshot; foliage, particles, cloth,
 * rigidbodies, fog volumes, water and lying snow all sample that same snapshot, so a rotor's
 * downwash reaches all of them without any of them knowing a rotor exists.
 *
 * Three wind samplers, because consumers genuinely want different answers:
 *
 *  - global_wind_at()  the weather's wind, gusts included -- identical to
 *                      toy::weather::wind_at(), which stays the authority on the global formula
 *  - local_wind_at()   the local sources alone, no gusts
 *  - wind_at()         both, folded by each source's EnvBlend. What most consumers want.
 *
 * A fog volume's drift deliberately uses the UNGUSTED mean plus locals, which is why the
 * split exists rather than one all-in sampler.
 *
 * Header-only, pure and trivially copyable: particle jobs and the physics substep callback
 * read snapshots of it with no locks. Its GPU twin is `gfx_env_*` in
 * assets/shaders/gfx/surface/world.glsl -- **the two must stay in step**, exactly as
 * toyengine/weather/wind.h and `gfx_world_wind_at()` already do.
 */

#ifndef TOYENGINE_ENV_ENV_FIELD_H
#define TOYENGINE_ENV_ENV_FIELD_H

#include <algorithm>

#include <glm/glm.hpp>

#include <toyengine/env/env_source.h>
#include <toyengine/weather/wind.h>

namespace toy {
namespace env {

/**
 * @brief Local sources a frame can carry.
 *
 * Deliberately small. The GPU fold runs per vertex in the foliage G-buffer stages AND in
 * their shadow and shadow-cube twins, over tens of thousands of grass instances, so the loop
 * bound is a real cost. Eight is more than any one view needs; `EnvSystem` keeps the
 * strongest and logs what it drops.
 */
inline constexpr int kMaxEnvSources = 8;

/** @brief Everything a point in the world can be asked about. */
struct EnvSample {
    glm::vec3 wind{0.0f};      ///< m/s, gusts and local sources included.
    float turbulence = 0.0f;   ///< 0..1 from local sources.
    float wetness = 0.0f;      ///< 0..1 surface wetness.
    float temperature = 18.0f; ///< Degrees C.
};

/** @brief The global weather term plus the frame's local sources. */
struct EnvField {
    // --- The global term (toyengine/weather/, zero when a scene has no weather) ---
    glm::vec3 wind{0.0f};      ///< Mean wind, m/s, horizontal.
    float wind_gust = 0.0f;    ///< Gust strength, 0 steady .. 1 calm-to-double.
    float wind_time = 0.0f;    ///< The gust clock (s).
    float wetness = 0.0f;
    float temperature = 18.0f;

    // --- Local sources ---
    int count = 0;
    EnvSource source[kMaxEnvSources]{};
};

/** @brief Whether `f` carries any local influence at all (the cheap early-out every consumer wants). */
inline bool has_sources(const EnvField& f) { return f.count > 0; }

/** @brief Whether `f` would move anything -- a global wind, or any local source. */
inline bool is_windy(const EnvField& f) {
    return glm::dot(f.wind, f.wind) > 1e-6f || f.count > 0;
}

/**
 * @brief The weather's wind (m/s) at `p`, gusts included; no local sources.
 *
 * @param f The field.
 * @param p The world point.
 * @return glm::vec3 The global wind there.
 */
inline glm::vec3 global_wind_at(const EnvField& f, glm::vec3 p) {
    return weather::wind_at(f.wind, f.wind_gust, f.wind_time, p);
}

/**
 * @brief The local sources' wind (m/s) at `p`; no weather, no gusts.
 *
 * `Add` sources sum. Of the `Max` sources only the strongest contributes, so overlapping
 * gusts do not stack into a hurricane. `Override` is not applied here -- it replaces a
 * combined result, so it belongs to wind_at().
 *
 * @param f The field.
 * @param p The world point.
 * @return glm::vec3 The local contribution; zero with no sources in range.
 */
inline glm::vec3 local_wind_at(const EnvField& f, glm::vec3 p) {
    glm::vec3 added(0.0f), strongest(0.0f);
    float strongest_len2 = 0.0f;
    for (int i = 0; i < f.count; ++i) {
        const EnvSource& s = f.source[i];
        if (s.blend == EnvBlend::Override) continue;
        const glm::vec3 w = env_wind(s, p);
        if (s.blend == EnvBlend::Add) {
            added += w;
        } else {
            const float len2 = glm::dot(w, w);
            if (len2 > strongest_len2) { strongest_len2 = len2; strongest = w; }
        }
    }
    return added + strongest;
}

/**
 * @brief The wind (m/s) at `p`: the weather, its gusts and every local source.
 *
 * `Override` sources are applied last. Of several overlapping ones the one reaching `p` most
 * strongly wins -- so the answer never depends on the order they were gathered in -- and its
 * own falloff eases back to the combined wind at its edge.
 *
 * @param f The field.
 * @param p The world point.
 * @return glm::vec3 The wind there.
 */
inline glm::vec3 wind_at(const EnvField& f, glm::vec3 p) {
    const glm::vec3 base = global_wind_at(f, p) + local_wind_at(f, p);
    const EnvSource* over = nullptr;
    float over_w = 0.0f;
    for (int i = 0; i < f.count; ++i) {
        const EnvSource& s = f.source[i];
        if (s.blend != EnvBlend::Override) continue;
        const float w = env_falloff(s, p);
        if (w > over_w) { over_w = w; over = &s; }
    }
    if (!over) return base;
    return base * (1.0f - over_w) + env_wind_full(*over, p) * over_w;
}

/** @brief Folds one scalar channel the same way wind_at() folds the wind. */
inline float env_fold_scalar_(const EnvField& f, glm::vec3 p, float base, float EnvSource::*channel) {
    float added = 0.0f, strongest = 0.0f;
    const EnvSource* over = nullptr;
    float over_w = 0.0f;
    for (int i = 0; i < f.count; ++i) {
        const EnvSource& s = f.source[i];
        const float w = env_falloff(s, p);
        if (w <= 0.0f) continue;
        const float v = s.*channel;
        if (s.blend == EnvBlend::Override) {
            if (w > over_w) { over_w = w; over = &s; }
        } else if (s.blend == EnvBlend::Add) {
            added += v * w;
        } else if (std::abs(v * w) > std::abs(strongest)) {
            strongest = v * w;
        }
    }
    const float combined = base + added + strongest;
    if (!over) return combined;
    return combined * (1.0f - over_w) + (over->*channel) * over_w;
}

/**
 * @brief Surface wetness at `p`, 0..1: the weather's plus every local source's.
 *
 * @param f The field.
 * @param p The world point.
 * @return float Clamped to 0..1, so a local dry zone (a negative wetness) cannot go below it.
 */
inline float wetness_at(const EnvField& f, glm::vec3 p) {
    return std::clamp(env_fold_scalar_(f, p, f.wetness, &EnvSource::wetness), 0.0f, 1.0f);
}

/**
 * @brief Temperature at `p` in degrees C: the weather's, offset by local sources.
 *
 * @param f The field.
 * @param p The world point.
 * @return float Degrees C.
 */
inline float temperature_at(const EnvField& f, glm::vec3 p) {
    return env_fold_scalar_(f, p, f.temperature, &EnvSource::temperature);
}

/**
 * @brief Extra flutter / noise at `p`, 0..1. Local sources only -- the weather's own gusts
 *        are already in the wind.
 *
 * @param f The field.
 * @param p The world point.
 * @return float Clamped to 0..1.
 */
inline float turbulence_at(const EnvField& f, glm::vec3 p) {
    return std::clamp(env_fold_scalar_(f, p, 0.0f, &EnvSource::turbulence), 0.0f, 1.0f);
}

/**
 * @brief Everything at `p` in one call.
 *
 * Convenience for gameplay. Per-particle and per-vertex code should call the single channel
 * it needs instead of paying for the ones it ignores.
 *
 * @param f The field.
 * @param p The world point.
 * @return EnvSample The wind, turbulence, wetness and temperature there.
 */
inline EnvSample sample(const EnvField& f, glm::vec3 p) {
    EnvSample s;
    s.wind = wind_at(f, p);
    s.turbulence = turbulence_at(f, p);
    s.wetness = wetness_at(f, p);
    s.temperature = temperature_at(f, p);
    return s;
}

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_ENV_FIELD_H
