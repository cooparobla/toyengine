/**
 * @file env_source.h
 * @brief One local environmental influence: a patch of the world where the wind, wetness,
 *        temperature or ground state differs from the scene-global weather.
 *
 * A rotor's downwash, a steam vent's updraft, a sword swing's gust, a leaking pipe's wet
 * ground. Sources are gathered into an `EnvField` once a frame (toyengine/env/env_field.h)
 * and sampled positionally by everything that reacts to the weather, so one source reaches
 * foliage, particles, cloth, rigidbodies, water and lying snow at once.
 *
 * A source is a plain POD with no owner: the `EnvInfluence` component builds one from its
 * transform each frame, and `EnvPulse` builds a short-lived one from code. Keep it trivially
 * copyable -- particle jobs and the physics substep read snapshots of it without locks, and
 * its GPU twin (`GfxEnvSource` in assets/shaders/gfx/surface/world.glsl) is a byte-for-byte
 * packing of the same numbers.
 *
 * `strength` is NOT stored here. The gather pre-multiplies a component's `strength` and a
 * pulse's envelope into the channel values, so sampling is branch-free on both CPU and GPU.
 */

#ifndef TOYENGINE_ENV_ENV_SOURCE_H
#define TOYENGINE_ENV_ENV_SOURCE_H

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace env {

/** @brief The volume a source fills. */
enum class EnvShape : int {
    Sphere = 0,  ///< A ball of `radius` about `position`.
    Disc   = 1,  ///< A cylinder of `radius` about the +Z axis through `position`, `height` tall
                 ///< either side of it (a rotor's column, a vent's plume).
};

/** @brief How a source's wind is aimed. */
enum class EnvMode : int {
    Directional = 0,  ///< One vector everywhere inside the shape (`wind`).
    Radial      = 1,  ///< Aimed away from the shape's +Z axis (`outward`) and along it
                      ///< (`vertical`), so one source expresses both a downwash
                      ///< (vertical < 0) and an updraft (vertical > 0).
};

/** @brief How a source combines with the weather and with other sources. */
enum class EnvBlend : int {
    Add      = 0,  ///< Summed on top of everything else: a gust over the prevailing wind.
    Max      = 1,  ///< The stronger of the two wins, so overlapping sources do not stack.
    Override = 2,  ///< Replaces the weather and every Add/Max source inside the shape, faded
                   ///< by the falloff: a walled courtyard the wind does not reach. Of several
                   ///< overlapping Overrides the one with the greatest influence wins, so the
                   ///< result never depends on gather order.
};

/**
 * @brief One local influence. Every channel is 0 / zero by default, meaning "contributes
 *        nothing": a source only does what it is given values for.
 */
struct EnvSource {
    // --- Shape ---
    glm::vec3 position{0.0f};  ///< World centre.
    float     radius = 6.0f;   ///< Metres; the influence is exactly zero at and beyond it.
    float     height = 0.0f;   ///< Disc: metres either side of `position.z`. 0 = unbounded in Z.
    float     falloff = 0.5f;  ///< 0 a hard edge .. 1 a soft bowl, as a fraction of `radius`.
    EnvShape  shape = EnvShape::Sphere;
    EnvBlend  blend = EnvBlend::Add;

    // --- Wind ---
    EnvMode   mode = EnvMode::Directional;
    glm::vec3 wind{0.0f};      ///< Directional: m/s.
    float     outward = 0.0f;  ///< Radial: m/s away from the axis, at the rim.
    float     vertical = 0.0f; ///< Radial: m/s along +Z. Negative is a downwash.
    float     turbulence = 0.0f;  ///< 0..1 extra flutter on foliage, extra noise on particles.

    // --- Surface and ground ---
    float wetness = 0.0f;      ///< 0..1 added surface wetness.
    float temperature = 0.0f;  ///< Degrees C offset (CPU only -- nothing on the GPU reads it).
    float snow = 0.0f;         ///< 0..1 lying-snow cover added here, whatever the weather is doing.
    float snow_clear = 0.0f;   ///< 0..1 how hard it scours lying snow (the subtractive twin).
    float water_disturb = 0.0f;  ///< 0..1 ripple rings on water beneath it.
};

/**
 * @brief How strongly `s` reaches world point `p`, 0..1.
 *
 * Exactly 1 inside `radius * (1 - falloff)` and exactly 0 at and beyond `radius`, with a
 * smooth ease between -- the same disc curve `toy::world::SnowField::stamp()` presses, so a
 * source's wind and the trench it scours share one edge.
 *
 * @param s The source.
 * @param p The world point.
 * @return float The 0..1 weight.
 */
inline float env_falloff(const EnvSource& s, glm::vec3 p) {
    if (s.radius <= 0.0f) return 0.0f;
    const glm::vec3 d = p - s.position;
    float r;
    if (s.shape == EnvShape::Disc) {
        // Radially a disc, with a hard cap in Z (height 0 = no cap, an infinite column).
        if (s.height > 0.0f && std::abs(d.z) > s.height) return 0.0f;
        r = std::sqrt(d.x * d.x + d.y * d.y);
    } else {
        r = std::sqrt(glm::dot(d, d));
    }
    if (r >= s.radius) return 0.0f;
    const float inner = s.radius * (1.0f - std::clamp(s.falloff, 0.0f, 1.0f));
    if (r <= inner) return 1.0f;
    const float t = (s.radius - r) / std::max(s.radius - inner, 1e-6f);
    return t * t * (3.0f - 2.0f * t);   // smoothstep
}

/**
 * @brief The wind (m/s) `s` would contribute at world point `p` at full strength, with no
 *        falloff applied.
 *
 * Radial sources aim away from the shape's +Z axis, so the direction follows the sample
 * point: a downwash fans outward under its centre, an updraft lifts along it.
 *
 * @param s The source.
 * @param p The world point.
 * @return glm::vec3 The unfaded contribution.
 */
inline glm::vec3 env_wind_full(const EnvSource& s, glm::vec3 p) {
    if (s.mode == EnvMode::Directional) return s.wind;
    glm::vec2 radial(p.x - s.position.x, p.y - s.position.y);
    const float len = std::sqrt(glm::dot(radial, radial));
    // On the axis there is no outward direction; the vertical term still applies.
    const glm::vec2 dir = len > 1e-4f ? radial / len : glm::vec2(0.0f);
    return glm::vec3(dir * s.outward, s.vertical);
}

/**
 * @brief The wind (m/s) `s` contributes at world point `p`, scaled by env_falloff().
 *
 * @param s The source.
 * @param p The world point.
 * @return glm::vec3 The contribution; zero outside the shape.
 */
inline glm::vec3 env_wind(const EnvSource& s, glm::vec3 p) {
    const float w = env_falloff(s, p);
    return w > 0.0f ? env_wind_full(s, p) * w : glm::vec3(0.0f);
}

/**
 * @brief A one-shot source with an envelope: born, held, then faded away.
 *
 * What a sword swing, an explosion's shockwave or a slammed door emits. `EnvSystem` ages it
 * and drops it when it expires, so emitting is fire-and-forget.
 */
struct EnvPulse {
    EnvSource source;      ///< The influence at full strength.
    float attack = 0.05f;  ///< Seconds from nothing to full.
    float hold = 0.05f;    ///< Seconds at full.
    float decay = 0.35f;   ///< Seconds back to nothing.
};

/**
 * @brief A pulse's 0..1 strength `age` seconds after it was emitted.
 *
 * @param p The pulse.
 * @param age Seconds since emission.
 * @return float 0 before it starts and once it has expired.
 */
inline float env_pulse_strength(const EnvPulse& p, float age) {
    if (age < 0.0f) return 0.0f;
    if (age < p.attack) return p.attack > 0.0f ? age / p.attack : 1.0f;
    const float held = age - p.attack;
    if (held < p.hold) return 1.0f;
    const float faded = held - p.hold;
    if (p.decay <= 0.0f || faded >= p.decay) return 0.0f;
    return 1.0f - faded / p.decay;
}

/** @brief Total seconds a pulse is alive. */
inline float env_pulse_lifetime(const EnvPulse& p) {
    return std::max(p.attack, 0.0f) + std::max(p.hold, 0.0f) + std::max(p.decay, 0.0f);
}

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_ENV_SOURCE_H
