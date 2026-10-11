#pragma once

/**
 * @file wind.h
 * @brief The world's wind field: the weather's mean wind plus gusts that roll downwind.
 *
 * WeatherState::wind is the mean wind (m/s, horizontal); `wind_gust` is how strong the gusts
 * are (0 = steady, 1 = gusts from calm to twice the mean) and `wind_time` the gust clock. The
 * field at a point is
 *
 *     wind_at(p) = wind * max(0, 1 + gust * g(p, t)),   g in [-1, 1]
 *
 * where g is a sum of wave fronts that travel downwind at the wind's speed (so one gust visibly
 * crosses a field, the smoke and a flag in turn) plus a slow pulse everywhere at once. Its mean
 * over space and time is 0, so the wind averages to `wind`.
 *
 * Everything that reacts to the wind reads this one field: foliage (assets/shaders/gfx/surface/
 * world.glsl gfx_world_wind_at -- the SAME formula; keep the two in step), particles
 * (ParticleSettings::wind_influence), fog volumes, water waves, cloth and rigidbodies
 * (toyengine/weather/wind_system.h). See toyengine/weather/README.md "Wind".
 */

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace toy {
namespace weather {

/** @brief The gust term g in [-1, 1] at world xy for a mean wind `wind` (m/s) at gust time `t` (s). */
inline float wind_gust_term(glm::vec2 wind, glm::vec2 xy, float t) {
    const float speed = glm::length(wind);
    const glm::vec2 dir = speed > 1e-4f ? wind / speed : glm::vec2(1.0f, 0.0f);
    const float along = glm::dot(xy, dir);
    const float cross = glm::dot(xy, glm::vec2(-dir.y, dir.x));
    const float a = along - speed * t;   // fronts move downwind with the air
    return 0.45f * std::sin(0.05f * a + 1.5f * std::sin(0.021f * cross)) +
           0.25f * std::sin(0.13f * a + 0.09f * cross + 2.1f) +
           0.30f * (0.6f * std::sin(0.7f * t) + 0.4f * std::sin(1.9f * t + 1.3f));
}

/** @brief The wind (m/s, horizontal) at world point `p`: mean `wind`, gust strength `gust`, gust clock `t`. */
inline glm::vec3 wind_at(glm::vec3 wind, float gust, float t, glm::vec3 p) {
    if (glm::dot(wind, wind) < 1e-8f) return glm::vec3(0.0f);
    const float g = gust > 0.0f ? wind_gust_term(glm::vec2(wind), glm::vec2(p), t) : 0.0f;
    return wind * std::max(0.0f, 1.0f + gust * g);
}

} // namespace weather
} // namespace toy
