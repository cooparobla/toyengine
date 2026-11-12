/**
 * @file env_field_test.cpp
 * @brief The environment field (toyengine/env/env_field.h): a field with no local sources is
 *        exactly the weather's wind, the falloff reaches full strength inside its inner radius
 *        and exactly zero at the rim, a radial source fans outward under its centre, Add / Max
 *        / Override compose as documented, and a pulse's envelope rises, holds and expires.
 */

#include <cmath>

#include <glm/glm.hpp>

#include <toyengine/env/env_field.h>
#include <toyengine/env/env_source.h>
#include <toyengine/weather/wind.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("env_field");

using toy::env::EnvBlend;
using toy::env::EnvField;
using toy::env::EnvMode;
using toy::env::EnvPulse;
using toy::env::EnvShape;
using toy::env::EnvSource;

namespace {

/** @brief A field with a steady global wind and no local sources. */
EnvField breezy(glm::vec3 wind = glm::vec3(4.0f, 0.0f, 0.0f), float gust = 0.3f) {
    EnvField f;
    f.wind = wind;
    f.wind_gust = gust;
    f.wind_time = 5.0f;
    return f;
}

/** @brief A rotor downwash centred at `at`. */
EnvSource downwash(glm::vec3 at, float radius = 6.0f) {
    EnvSource s;
    s.position = at;
    s.radius = radius;
    s.falloff = 0.5f;
    s.shape = EnvShape::Sphere;
    s.mode = EnvMode::Radial;
    s.outward = 12.0f;
    s.vertical = -16.0f;
    return s;
}

} // namespace

COOPA_TEST(no_sources_is_exactly_the_weather) {
    const EnvField f = breezy();
    for (float x = -40.0f; x <= 40.0f; x += 7.0f) {
        const glm::vec3 p(x, x * 0.5f, 1.0f);
        expect(toy::env::wind_at(f, p) == toy::weather::wind_at(f.wind, f.wind_gust, f.wind_time, p),
               "env: with no local sources the field is the weather's wind, unchanged");
    }
    expect(toy::env::local_wind_at(f, glm::vec3(0.0f)) == glm::vec3(0.0f),
           "env: no sources means no local wind anywhere");
    expect(!toy::env::has_sources(f), "env: an empty field reports no sources");
    expect(toy::env::is_windy(f), "env: a global wind alone still counts as windy");
}

COOPA_TEST(a_calm_field_with_one_source_is_windy) {
    EnvField f;   // no weather at all
    f.source[f.count++] = downwash(glm::vec3(0.0f, 0.0f, 5.0f));
    expect(toy::env::is_windy(f),
           "env: a local source alone makes the field windy -- local influences must work in a "
           "scene with no weather");
    expect(glm::length(toy::env::wind_at(f, glm::vec3(2.0f, 0.0f, 5.0f))) > 1.0f,
           "env: the source moves air with the weather switched off");
}

COOPA_TEST(falloff_is_full_inside_and_exactly_zero_at_the_rim) {
    EnvSource s = downwash(glm::vec3(0.0f));
    s.radius = 10.0f;
    s.falloff = 0.4f;   // full strength inside 6 m
    expect(toy::env::env_falloff(s, glm::vec3(0.0f)) == 1.0f, "env: full strength at the centre");
    expect(toy::env::env_falloff(s, glm::vec3(5.9f, 0.0f, 0.0f)) == 1.0f,
           "env: full strength anywhere inside radius * (1 - falloff)");
    expect(toy::env::env_falloff(s, glm::vec3(10.0f, 0.0f, 0.0f)) == 0.0f,
           "env: exactly zero at the radius -- no influence leaks past the shape");
    expect(toy::env::env_falloff(s, glm::vec3(50.0f, 0.0f, 0.0f)) == 0.0f, "env: zero far outside");
    const float mid = toy::env::env_falloff(s, glm::vec3(8.0f, 0.0f, 0.0f));
    expect(mid > 0.0f && mid < 1.0f, "env: the rim eases rather than stepping");
    // Monotone across the falloff band: no ringing.
    float prev = 1.0f;
    for (float r = 6.0f; r <= 10.0f; r += 0.25f) {
        const float v = toy::env::env_falloff(s, glm::vec3(r, 0.0f, 0.0f));
        expect(v <= prev + 1e-6f, "env: the falloff never rises as you move outward");
        prev = v;
    }
}

COOPA_TEST(a_disc_is_capped_in_z_and_a_sphere_is_not) {
    EnvSource d = downwash(glm::vec3(0.0f, 0.0f, 10.0f));
    d.shape = EnvShape::Disc;
    d.radius = 5.0f;
    d.height = 3.0f;
    expect(toy::env::env_falloff(d, glm::vec3(0.0f, 0.0f, 10.0f)) == 1.0f, "env: inside the disc");
    expect(toy::env::env_falloff(d, glm::vec3(0.0f, 0.0f, 14.0f)) == 0.0f,
           "env: a disc's height caps it in Z -- nothing above a vent's plume");
    EnvSource inf = d;
    inf.height = 0.0f;
    expect(toy::env::env_falloff(inf, glm::vec3(0.0f, 0.0f, 400.0f)) == 1.0f,
           "env: height 0 is an unbounded column");
    EnvSource sph = d;
    sph.shape = EnvShape::Sphere;
    expect(toy::env::env_falloff(sph, glm::vec3(0.0f, 0.0f, 14.0f)) > 0.0f,
           "env: a sphere of the same radius still reaches 4 m up -- it falls off in Z by "
           "distance, so the disc's cap is what cut the plume off");
    expect(toy::env::env_falloff(sph, glm::vec3(0.0f, 0.0f, 16.0f)) == 0.0f,
           "env: ...and the sphere is zero once the distance passes its radius");
}

COOPA_TEST(a_radial_source_fans_outward_and_presses_down) {
    EnvField f;
    f.source[f.count++] = downwash(glm::vec3(0.0f, 0.0f, 0.0f));

    const glm::vec3 east = toy::env::wind_at(f, glm::vec3(3.0f, 0.0f, 0.0f));
    const glm::vec3 west = toy::env::wind_at(f, glm::vec3(-3.0f, 0.0f, 0.0f));
    expect(east.x > 0.0f, "env: a downwash blows east of its centre toward the east");
    expect(west.x < 0.0f, "env: ...and west of it toward the west -- it fans out, not one way");
    expect_near(east.x, -west.x, 1e-4f, "env: the fan is symmetric about the axis");
    expect(east.z < 0.0f && west.z < 0.0f,
           "env: a negative `vertical` presses down everywhere under the rotor");

    const glm::vec3 axis = toy::env::wind_at(f, glm::vec3(0.0f, 0.0f, 0.0f));
    expect_near(axis.x, 0.0f, 1e-5f, "env: on the axis there is no outward direction to pick");
    expect(axis.z < 0.0f, "env: ...but the downward push still applies there");

    // An updraft is the same source with the sign flipped -- one component expresses both.
    EnvField vent;
    EnvSource v = downwash(glm::vec3(0.0f));
    v.outward = 1.5f;
    v.vertical = 9.0f;
    vent.source[vent.count++] = v;
    expect(toy::env::wind_at(vent, glm::vec3(1.0f, 0.0f, 0.0f)).z > 0.0f,
           "env: a positive `vertical` lifts -- a vent, not a rotor");
}

COOPA_TEST(add_stacks_max_does_not_and_override_wins) {
    const glm::vec3 p(0.0f, 0.0f, 0.0f);

    EnvSource a;
    a.position = p;
    a.radius = 10.0f;
    a.falloff = 0.0f;          // hard edge, so the weight at p is exactly 1
    a.mode = EnvMode::Directional;
    a.wind = glm::vec3(3.0f, 0.0f, 0.0f);
    EnvSource b = a;
    b.wind = glm::vec3(5.0f, 0.0f, 0.0f);

    {   // Add: the two sum, on top of the weather.
        EnvField f = breezy(glm::vec3(2.0f, 0.0f, 0.0f), 0.0f);
        f.source[f.count++] = a;
        f.source[f.count++] = b;
        expect_near(toy::env::wind_at(f, p).x, 2.0f + 3.0f + 5.0f, 1e-4f,
                    "env: Add sources sum with each other and with the weather");
    }
    {   // Max: only the stronger of the two contributes.
        EnvField f = breezy(glm::vec3(2.0f, 0.0f, 0.0f), 0.0f);
        a.blend = EnvBlend::Max;
        b.blend = EnvBlend::Max;
        f.source[f.count++] = a;
        f.source[f.count++] = b;
        expect_near(toy::env::wind_at(f, p).x, 2.0f + 5.0f, 1e-4f,
                    "env: overlapping Max sources do not stack -- the strongest wins");
    }
    {   // Override: replaces the weather and the Add source inside the shape.
        EnvField f = breezy(glm::vec3(2.0f, 0.0f, 0.0f), 0.0f);
        EnvSource shelter = a;
        shelter.blend = EnvBlend::Override;
        shelter.wind = glm::vec3(0.0f);
        a.blend = EnvBlend::Add;
        f.source[f.count++] = a;
        f.source[f.count++] = shelter;
        expect_near(toy::env::wind_at(f, p).x, 0.0f, 1e-4f,
                    "env: an Override of zero wind is a sheltered pocket -- the weather and the "
                    "other sources do not reach inside it");
        expect_near(toy::env::wind_at(f, glm::vec3(40.0f, 0.0f, 0.0f)).x, 2.0f, 1e-4f,
                    "env: ...and outside its radius the weather is untouched");
    }
}

COOPA_TEST(override_does_not_depend_on_gather_order) {
    // Two overlapping Overrides; the one reaching p more strongly must win either way round.
    EnvSource near_strong;
    near_strong.position = glm::vec3(0.0f);
    near_strong.radius = 10.0f;
    near_strong.falloff = 0.5f;
    near_strong.blend = EnvBlend::Override;
    near_strong.mode = EnvMode::Directional;
    near_strong.wind = glm::vec3(7.0f, 0.0f, 0.0f);

    EnvSource far_weak = near_strong;
    far_weak.position = glm::vec3(9.0f, 0.0f, 0.0f);
    far_weak.wind = glm::vec3(-7.0f, 0.0f, 0.0f);

    EnvField one = breezy(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f);
    one.source[one.count++] = near_strong;
    one.source[one.count++] = far_weak;

    EnvField other = breezy(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f);
    other.source[other.count++] = far_weak;
    other.source[other.count++] = near_strong;

    const glm::vec3 p(1.0f, 0.0f, 0.0f);
    expect_near(toy::env::wind_at(one, p).x, toy::env::wind_at(other, p).x, 1e-5f,
                "env: overlapping Overrides resolve by influence, not by gather order -- "
                "otherwise the wind would flicker as objects are reordered");
}

COOPA_TEST(scalar_channels_fall_off_like_the_wind) {
    EnvField f;
    f.wetness = 0.0f;
    f.temperature = 10.0f;
    EnvSource wet;
    wet.position = glm::vec3(0.0f);
    wet.radius = 5.0f;
    wet.falloff = 0.5f;
    wet.wetness = 0.9f;
    wet.temperature = 20.0f;
    wet.turbulence = 0.5f;
    f.source[f.count++] = wet;

    expect_near(toy::env::wetness_at(f, glm::vec3(0.0f)), 0.9f, 1e-4f, "env: wet at the centre");
    expect_near(toy::env::wetness_at(f, glm::vec3(5.0f, 0.0f, 0.0f)), 0.0f, 1e-4f,
                "env: dry at the rim -- a local wet patch has an edge");
    expect_near(toy::env::temperature_at(f, glm::vec3(0.0f)), 30.0f, 1e-4f,
                "env: temperature is an offset on the ambient, not a replacement");
    expect_near(toy::env::temperature_at(f, glm::vec3(20.0f, 0.0f, 0.0f)), 10.0f, 1e-4f,
                "env: ...and outside the shape the ambient stands");
    expect_near(toy::env::turbulence_at(f, glm::vec3(0.0f)), 0.5f, 1e-4f, "env: turbulence folds too");

    // Wetness is clamped, so a dry zone cannot drive it negative.
    EnvField dry;
    dry.wetness = 0.4f;
    EnvSource awning;
    awning.position = glm::vec3(0.0f);
    awning.radius = 5.0f;
    awning.falloff = 0.0f;
    awning.wetness = -1.0f;
    dry.source[dry.count++] = awning;
    expect(toy::env::wetness_at(dry, glm::vec3(0.0f)) == 0.0f,
           "env: a local dry zone bottoms out at 0, never negative");
}

COOPA_TEST(the_snow_channel_lies_snow_without_any_weather) {
    // The additive twin of snow_clear. It is NOT folded with the weather's own cover here --
    // the shaders combine the two with max() -- so this reads 0 outside a source whatever the
    // sky is doing, which is what lets a pile exist in a snowless scene.
    EnvField f;
    EnvSource pile;
    pile.position = glm::vec3(0.0f);
    pile.radius = 5.0f;
    pile.falloff = 0.5f;
    pile.snow = 0.9f;
    f.source[f.count++] = pile;

    expect_near(toy::env::snow_at(f, glm::vec3(0.0f)), 0.9f, 1e-4f, "snow: full cover at the centre");
    expect_near(toy::env::snow_at(f, glm::vec3(5.0f, 0.0f, 0.0f)), 0.0f, 1e-4f,
                "snow: nothing at the rim -- a pile has an edge");
    expect(toy::env::snow_at(f, glm::vec3(40.0f, 0.0f, 0.0f)) == 0.0f, "snow: nothing far away");
    const float mid = toy::env::snow_at(f, glm::vec3(4.0f, 0.0f, 0.0f));
    expect(mid > 0.0f && mid < 0.9f, "snow: it eases out rather than stepping");

    // snow and snow_clear are independent channels, not two ends of one.
    EnvField both;
    EnvSource scour = pile;
    scour.snow = 0.0f;
    scour.snow_clear = 1.0f;
    both.source[both.count++] = scour;
    expect(toy::env::snow_at(both, glm::vec3(0.0f)) == 0.0f,
           "snow: a scouring source lays none -- the two channels do not alias");
}

COOPA_TEST(a_pulse_rises_holds_and_expires) {
    EnvPulse p;
    p.source = downwash(glm::vec3(0.0f));
    p.attack = 0.1f;
    p.hold = 0.2f;
    p.decay = 0.4f;

    expect(toy::env::env_pulse_strength(p, 0.0f) == 0.0f, "pulse: nothing at the instant it is emitted");
    expect_near(toy::env::env_pulse_strength(p, 0.05f), 0.5f, 1e-4f, "pulse: half way up the attack");
    expect(toy::env::env_pulse_strength(p, 0.1f) == 1.0f, "pulse: full at the end of the attack");
    expect(toy::env::env_pulse_strength(p, 0.25f) == 1.0f, "pulse: still full through the hold");
    expect_near(toy::env::env_pulse_strength(p, 0.5f), 0.5f, 1e-4f, "pulse: half way down the decay");
    // At exactly attack + hold + decay the float arithmetic lands within an epsilon of zero;
    // EnvSystem drops a pulse on age >= lifetime, so the boundary value is never sampled.
    expect_near(toy::env::env_pulse_strength(p, 0.7f), 0.0f, 1e-4f,
                "pulse: spent by the end of the decay");
    expect(toy::env::env_pulse_strength(p, 5.0f) == 0.0f, "pulse: and it stays expired");
    expect_near(toy::env::env_pulse_lifetime(p), 0.7f, 1e-5f, "pulse: lifetime is attack + hold + decay");

    // Monotone up then monotone down -- a swing that snaps and recovers, with no second kick.
    float peak_t = 0.0f, peak = 0.0f;
    for (float t = 0.0f; t <= 0.7f; t += 0.01f) {
        const float v = toy::env::env_pulse_strength(p, t);
        if (v > peak) { peak = v; peak_t = t; }
    }
    expect_near(peak, 1.0f, 1e-4f, "pulse: it does reach full strength");
    expect(peak_t <= 0.31f, "pulse: the peak is in the attack/hold, not late in the decay");
}
