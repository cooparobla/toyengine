/**
 * @file sky_model.h
 * @brief Time of day -> sun / moon positions, the scene's directional light and the sky
 *        gradient. Pure math, no scene: weather_system.h layers the active condition on top.
 *
 * Sun path. The engine is Z-up; with north_offset 0 the sun rises toward +X (east) at 06:00,
 * peaks at noon toward -Y (south, for a positive latitude) at an elevation of 90 - latitude
 * degrees, and sets toward -X at 18:00 -- an equinox day, so day and night are 12 hours each.
 * With hour angle H = (hour - 12) * 15 degrees and latitude phi, the direction TO the sun is
 *
 *     east = -sin H,   north = -cos H sin phi,   up = cos H cos phi
 *
 * then turned about Z by north_offset. The moon is the opposite point of the sky.
 *
 * One directional light. The scene's directional light follows the sun while it is up and the
 * moon while it is down. Each fades to zero intensity at the hand-over (the sun a little below
 * the horizon), so the swap of direction is never visible: the shadows fade out, turn, and fade
 * back in. The sun's colour warms toward the horizon (more air to cross).
 *
 * Sky gradient. The settings' day / twilight / night palettes are blended by the sun's height:
 * night below about -15 degrees, day above about +15, and a twilight band around the horizon that
 * colours mostly the horizon (sunrise / sunset glow) and a little of the zenith.
 */

#ifndef TOYENGINE_WEATHER_SKY_MODEL_H
#define TOYENGINE_WEATHER_SKY_MODEL_H

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include <toyengine/weather/weather_profile.h>

namespace toy {
namespace weather {

/** @brief The clock's view of the sky at one moment, before any weather. */
struct SkyFrame {
    glm::vec3 sun_to{0.0f, 0.0f, 1.0f};    ///< Unit direction TO the sun.
    glm::vec3 moon_to{0.0f, 0.0f, -1.0f};  ///< Unit direction TO the moon.
    float sun_height = 1.0f;               ///< sin(sun elevation): 1 overhead, 0 on the horizon, < 0 below.
    float daylight = 1.0f;                 ///< 0 night .. 1 full day.
    float twilight = 0.0f;                 ///< 0..1, peaking with the sun on the horizon.

    glm::vec3 zenith{0.0f}, horizon{0.0f}, ground{0.0f};
    float ambient = 1.0f;                  ///< ambient_intensity / sky_intensity.

    // The scene's directional light (sun or moon).
    glm::vec3 light_dir{0.0f, 0.0f, -1.0f};  ///< Direction the rays travel (FROM the light).
    glm::vec3 light_color{1.0f};
    float light_intensity = 1.0f;
    bool moon_light = false;               ///< The light is the moon.
};

namespace sky_detail {
float smoothstep(float e0, float e1, float x);
/** @brief Sun colour by height: deep orange at the horizon to near white overhead. */
glm::vec3 sun_color(float h);
} // namespace sky_detail

/** @brief Height of the sun's hand-over to the moon (sin of about -2.3 degrees). */
inline constexpr float k_light_swap_height = -0.04f;

/** @brief Direction TO the sun at `hour` (see the file doc). */
glm::vec3 sun_direction(float hour, float latitude_deg, float north_offset_deg);

/** @brief The clock's sky at `hour` for these settings (no weather applied). */
SkyFrame evaluate_sky(const Settings& st, float hour);

/** @brief Wraps hours into [0, 24). */
inline float wrap_hour(float h) { return std::fmod(std::fmod(h, 24.0f) + 24.0f, 24.0f); }

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_SKY_MODEL_H
