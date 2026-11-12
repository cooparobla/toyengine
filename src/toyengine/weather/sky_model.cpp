#include <toyengine/weather/sky_model.h>

namespace toy {
namespace weather {
namespace sky_detail {

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 sun_color(float h) {
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
} // namespace weather
} // namespace toy

namespace toy {
namespace weather {

glm::vec3 sun_direction(float hour, float latitude_deg, float north_offset_deg) {
    const float H = glm::radians((hour - 12.0f) * 15.0f);
    const float phi = glm::radians(latitude_deg);
    const glm::vec3 d(-std::sin(H), -std::cos(H) * std::sin(phi), std::cos(H) * std::cos(phi));
    const float a = glm::radians(north_offset_deg);
    const float c = std::cos(a), s = std::sin(a);
    return glm::normalize(glm::vec3(c * d.x - s * d.y, s * d.x + c * d.y, d.z));
}

SkyFrame evaluate_sky(const Settings& st, float hour) {
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

} // namespace weather
} // namespace toy
