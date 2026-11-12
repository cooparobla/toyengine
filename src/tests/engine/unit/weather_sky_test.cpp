/**
 * @file weather_sky_test.cpp
 * @brief The sky models on the CPU: the sun's path and the gradient sky's palettes follow the
 *        clock (no pop at the sun/moon hand-over), and the physical atmosphere model's colours
 *        (blue noon, red sunset, reddened low sun, dim night) with its tables built only on change.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/glm.hpp>
#include <toyengine/weather/atmosphere_model.h>
#include <toyengine/weather/sky_model.h>
#include <toyengine/weather/weather_profile.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("weather_sky");

COOPA_TEST(sun_path_and_sky_palette_follow_the_clock) {
    using namespace toy::weather;
    Settings st;
    st.latitude = 40.0f;
    st.north_offset = 0.0f;
    const glm::vec3 dawn = sun_direction(6.0f, st.latitude, 0.0f);
    expect_near(dawn.z, 0.0f, 1e-4f, "sky: the sun is on the horizon at 06:00");
    expect(dawn.x > 0.99f, "sky: ...rising toward +X (east)");
    const glm::vec3 noon = sun_direction(12.0f, st.latitude, 0.0f);
    expect_near(glm::degrees(std::asin(noon.z)), 50.0f, 0.01f, "sky: noon elevation is 90 - latitude");
    expect(noon.y < 0.0f, "sky: the noon sun is to the south at a northern latitude");
    expect(sun_direction(18.0f, st.latitude, 0.0f).x < -0.99f, "sky: it sets toward -X");
    const glm::vec3 turned = sun_direction(6.0f, st.latitude, 90.0f);
    expect(turned.y > 0.99f, "sky: north_offset turns the path about Z");

    const SkyFrame day = evaluate_sky(st, 12.0f), night = evaluate_sky(st, 0.0f);
    expect(!day.moon_light && night.moon_light, "sky: the light is the sun by day, the moon by night");
    expect(day.daylight > 0.99f && night.daylight < 0.01f, "sky: daylight runs 1 at noon to 0 at midnight");
    expect(glm::length(day.zenith - st.day_zenith) < 1e-3f, "sky: noon shows the day palette");
    expect(glm::length(night.zenith - st.night_zenith) < 1e-3f, "sky: midnight shows the night palette");
    expect(evaluate_sky(st, 18.1f).twilight > 0.5f, "sky: twilight peaks around sunset");
    expect_near(day.light_intensity, st.sun_intensity, 1e-3f, "sky: full sun intensity at noon");
    // The hand-over from sun to moon happens at zero intensity: no visible pop.
    float worst = 0.0f;
    for (float h = 17.5f; h < 19.5f; h += 0.002f) {
        worst = std::max(worst, std::abs(evaluate_sky(st, h + 0.002f).light_intensity - evaluate_sky(st, h).light_intensity));
    }
    expect(worst < 0.01f, "sky: the light's intensity is continuous through dusk (max step " + std::to_string(worst) + ")");
}

COOPA_TEST(atmosphere_model_colours_and_caches_its_tables) {
    using toy::weather::AtmosphereModel;
    AtmosphereModel m;
    expect(m.builds() == 1, "atmosphere: the tables are built once at construction");
    m.set_media(toy::render::SkyAtmosphereMedia{});
    expect(m.builds() == 1, "atmosphere: unchanged media do not rebuild the tables");
    const float vr = m.view_radius(2.0f);
    const glm::vec3 noon = glm::normalize(glm::vec3(0.3f, -0.5f, 0.8f));
    const auto day = m.gradient(noon, 0.025f, vr);
    expect(day.zenith.b > day.zenith.g && day.zenith.g > day.zenith.r, "atmosphere: the noon zenith is blue");
    const glm::vec3 sunset = glm::normalize(glm::vec3(-1.0f, 0.0f, 0.03f));
    const glm::vec3 toward = m.radiance(glm::normalize(glm::vec3(-1.0f, 0.0f, 0.04f)), sunset, 0.025f, vr);
    expect(toward.r > toward.b * 1.5f, "atmosphere: the horizon under a setting sun is red / orange");
    const glm::vec3 t_low = m.transmittance_toward(vr, sunset), t_up = m.transmittance(vr, 1.0f);
    expect(t_low.r / t_up.r > 2.0f * (t_low.b / t_up.b), "atmosphere: a low sun's light is reddened");
    expect(glm::length(m.transmittance_toward(vr, glm::vec3(0, 0, -1))) == 0.0f, "atmosphere: no light through the planet");
    const auto night = m.gradient(-noon, 0.025f, vr);
    expect(night.zenith.b < day.zenith.b * 0.1f && night.zenith.b > 0.0f, "atmosphere: the moonlit night is dim, not black");
    m.set_media(toy::render::SkyAtmosphereMedia::earth(4.0f, 1.0f));
    expect(m.builds() == 2, "atmosphere: new media rebuild the tables");
    const auto hazy = m.gradient(noon, 0.025f, vr);
    const auto sat = [](glm::vec3 c) { return (glm::max(c.r, glm::max(c.g, c.b)) - glm::min(c.r, glm::min(c.g, c.b))) / glm::max(c.r, glm::max(c.g, c.b)); };
    expect(sat(hazy.horizon) < sat(day.horizon), "atmosphere: haze whitens the horizon");
}
