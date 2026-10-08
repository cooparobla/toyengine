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
inline float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
/** @brief Sun colour by height: deep orange at the horizon to near white overhead. */
inline glm::vec3 sun_color(float h) {
    struct Key { float h; glm::vec3 c; };
    static const Key keys[] = {
        {-0.05f, {1.00f, 0.30f, 0.10f}}, {0.00f, {1.00f, 0.42f, 0.18f}}, {0.08f, {1.00f, 0.62f, 0.36f}},
        {0.25f, {1.00f, 0.86f, 0.70f}},  {0.50f, {1.00f, 0.95f, 0.88f}}, {1.00f, {1.00f, 0.97f, 0.93f}},
    };
    if (h <= keys[0].h) return keys[0].c;
    for (size_t i = 1; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (h <= keys[i].h) return glm::mix(keys[i - 1].c, keys[i].c, (h - keys[i - 1].h) / (keys[i].h - keys[i - 1].h));
    }
    return keys[5].c;
}
} // namespace sky_detail

/** @brief Height of the sun's hand-over to the moon (sin of about -2.3 degrees). */
inline constexpr float k_light_swap_height = -0.04f;

/** @brief Direction TO the sun at `hour` (see the file doc). */
inline glm::vec3 sun_direction(float hour, float latitude_deg, float north_offset_deg) {
    const float H = glm::radians((hour - 12.0f) * 15.0f);
    const float phi = glm::radians(latitude_deg);
    const glm::vec3 d(-std::sin(H), -std::cos(H) * std::sin(phi), std::cos(H) * std::cos(phi));
    const float a = glm::radians(north_offset_deg);
    const float c = std::cos(a), s = std::sin(a);
    return glm::normalize(glm::vec3(c * d.x - s * d.y, s * d.x + c * d.y, d.z));
}

/** @brief The clock's sky at `hour` for these settings (no weather applied). */
inline SkyFrame evaluate_sky(const Settings& st, float hour) {
    using sky_detail::smoothstep;
    SkyFrame f;
    f.sun_to = sun_direction(hour, st.latitude, st.north_offset);
    f.moon_to = -f.sun_to;
    const float h = f.sun_height = f.sun_to.z;

    f.daylight = smoothstep(-0.12f, 0.25f, h);
    f.twilight = std::exp(-((h - 0.02f) / 0.11f) * ((h - 0.02f) / 0.11f));
    const float tw = f.twilight;

    f.zenith = glm::mix(glm::mix(st.night_zenith, st.day_zenith, f.daylight), st.twilight_zenith, tw * 0.5f);
    f.horizon = glm::mix(glm::mix(st.night_horizon, st.day_horizon, f.daylight), st.twilight_horizon, tw * 0.85f);
    f.ground = glm::mix(st.night_ground, st.day_ground, f.daylight);
    f.ambient = glm::mix(st.ambient_night, st.ambient_day, f.daylight);

    if (h > k_light_swap_height) {
        f.moon_light = false;
        f.light_dir = -f.sun_to;
        f.light_color = sky_detail::sun_color(h);
        f.light_intensity = st.sun_intensity * smoothstep(k_light_swap_height, 0.12f, h);
    } else {
        f.moon_light = true;
        f.light_dir = -f.moon_to;
        f.light_color = st.moon_color;
        f.light_intensity = st.moon_intensity * smoothstep(-k_light_swap_height, 0.2f, -h);
    }
    return f;
}

/** @brief Wraps hours into [0, 24). */
inline float wrap_hour(float h) { return std::fmod(std::fmod(h, 24.0f) + 24.0f, 24.0f); }

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_SKY_MODEL_H
