/**
 * @file weather_system_test.cpp
 * @brief WeatherSystem on a bare Scene (execute() by hand): the YAML block and stock catalogue
 *        round-trip, conditions blend and interrupt without jumps, transitions fast-forward,
 *        an authored sun is driven and restored, the schedule and clock signals fire, and
 *        WeatherReactor's rules match.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/weather/weather_reactor.h>
#include <toyengine/weather/weather_system.h>
#include <gfxcoopa/engine/components/directional_light.h>

#include "engine/support/checks.h"
#include "engine/support/weather_fixtures.h"

COOPA_TEST_SUITE("weather_system");

using namespace toy::test;

COOPA_TEST(settings_round_trip_with_the_stock_catalogue) {
    using namespace toy::weather;
    const Settings none = parse_settings(fkyaml::node());
    expect(!none.enabled, "weather yaml: no block = disabled");
    expect(none.conditions.size() == default_conditions().size(), "weather yaml: no conditions = the stock catalogue");
    for (const char* n : {"clear", "cloudy", "overcast", "fog", "rain", "storm", "snow", "blizzard", "sandstorm"}) {
        expect(none.find(n) != nullptr, std::string("weather yaml: stock condition '") + n + "'");
    }
    const Settings st = parse_settings(weather_test_block("schedule: random\nlatitude: 12\n"));
    expect(st.enabled && st.schedule == Schedule::Random && st.conditions.size() == 3, "weather yaml: block parsed");
    const Condition* wet = st.find("wet");
    expect(wet && wet->min_minutes == 0.1f && wet->next.size() == 1 && wet->next[0] == "sunny", "weather yaml: duration and next");
    const Settings back = parse_settings(to_node(st));
    expect(back.schedule == st.schedule && back.latitude == st.latitude && back.conditions.size() == st.conditions.size() &&
           back.find("wet") && back.find("wet")->fog_density == wet->fog_density && back.find("wet")->precipitation == 1.0f,
           "weather yaml: to_node() -> parse_settings() round-trips");
    const Settings stock_back = parse_settings(to_node(none));
    const Condition* storm = stock_back.find("storm");
    expect(storm && storm->effects.size() == 2 && storm->effects[0].prefab == "objects/weather_rain" &&
           storm->effects[1].follow == EffectAnchor::Ground && storm->lightning > 0.0f, "weather yaml: effects round-trip");
    const Settings bad = parse_settings(fkyaml::node::deserialize(std::string("enabled: true\ncondition: nope\ntime_of_day: 27\n")));
    expect(bad.condition == bad.conditions.front().name, "weather yaml: an unknown start condition falls back to the first");
    expect_near(bad.time_of_day, 3.0f, 1e-4f, "weather yaml: time_of_day wraps into [0, 24)");
    expect(controls_render_key("fog_density") && controls_render_key("sky_zenith") && !controls_render_key("exposure") &&
           !controls_render_key("fog_enabled"), "weather: the render keys it locks");
}

COOPA_TEST(transitions_blend_interrupt_and_fast_forward_without_jumps) {
    {
        coopa::scene::Scene scene("weather");
        toy::weather::WeatherSystem* w = toy::weather::install_weather_system(scene, weather_test_block());
        weather_step(*w, scene, 0.5f);
        expect(w->state().enabled && w->state().condition == "sunny" && w->state().transition == 1.0f, "weather: starts in its condition");
        expect_near(w->atmosphere().fog_density, 0.0f, 1e-6f, "weather: sunny has no fog");

        std::string from, to;
        auto conn = w->on_condition_changed.connect_scoped([&](const std::string& a, const std::string& b) { from = a; to = b; });
        expect(w->set_condition("wet"), "weather: set_condition() accepts a known name");
        expect(!w->set_condition("nope"), "weather: ...and refuses an unknown one");
        expect(from == "sunny" && to == "wet", "weather: on_condition_changed(from, to)");
        weather_step(*w, scene, 5.0f);
        const float mid = w->atmosphere().fog_density;
        expect(mid > 0.005f && mid < 0.035f, "weather: half way through the transition the fog is part way (" + std::to_string(mid) + ")");
        expect(w->state().transition > 0.4f && w->state().transition < 0.6f, "weather: state().transition reports progress");
        // Interrupting goes back from where it is: no jump.
        w->set_condition("sunny");
        weather_step(*w, scene, 0.1f);
        expect(std::abs(w->atmosphere().fog_density - mid) < 0.004f, "weather: an interrupted transition starts from what was showing");
        weather_step(*w, scene, 10.5f);
        expect_near(w->atmosphere().fog_density, 0.0f, 1e-5f, "weather: ...and lands on the new condition");
        w->set_condition("wet", 0.0f);
        weather_step(*w, scene, 0.1f);
        expect_near(w->atmosphere().fog_density, 0.04f, 1e-5f, "weather: a 0 s transition snaps");
        expect_near(w->state().precipitation, 1.0f, 1e-5f, "weather: the state carries the profile");
        expect(w->state().wind_speed() > 1.0f, "weather: wind blows");

        // The sun: no authored light, so a runtime Sun under the locked Weather root.
        coopa::scene::SceneObject* root = w->root();
        expect(root && toy::scene::is_runtime_object(*root) && root->name() == "Weather", "weather: a marked runtime root");
        auto* sun = root ? root->find_child("Sun") : nullptr;
        auto* light = sun ? sun->get_component<coopa::gfx::engine::components::DirectionalLightComponent>() : nullptr;
        expect(light && toy::scene::is_runtime_object(*sun), "weather: a runtime Sun inherits the marker");
        if (light) expect(light->direction.z < 0.0f && light->intensity > 0.0f, "weather: the sun shines down at 11:30");

        // Disabled: everything it made goes away.
        toy::weather::Settings off = w->settings();
        off.enabled = false;
        w->set_settings(off);
        weather_step(*w, scene, 0.1f);
        expect(!w->state().enabled && w->root() == nullptr && scene.root_objects().empty(), "weather: disabling removes its runtime objects");
    }
    {
        coopa::scene::Scene scene("weather");
        toy::weather::WeatherSystem* w = toy::weather::install_weather_system(scene, weather_test_block());
        weather_step(*w, scene, 0.2f);
        w->set_condition("wet");                        // a 10 s transition
        w->set_transition_speed(4.0f);
        weather_step(*w, scene, 1.3f);
        expect(w->transitioning() && w->state().transition > 0.45f && w->state().transition < 0.6f,
               "transition speed: 4x covers half of a 10 s blend in 1.25 s");
        w->finish_transition();
        weather_step(*w, scene, 0.1f);
        expect(!w->transitioning() && std::abs(w->atmosphere().fog_density - 0.04f) < 1e-5f, "finish_transition lands on the target");
        w->set_transition_speed(0.0f);
        w->set_condition("sunny");
        weather_step(*w, scene, 0.1f);
        expect(!w->transitioning() && w->atmosphere().fog_density < 1e-5f, "transition speed 0 is instant");
    }
}

COOPA_TEST(drives_and_restores_an_authored_sun) {
    coopa::scene::Scene scene("weather");
    auto obj = std::make_unique<coopa::scene::SceneObject>("sun");
    auto* light = obj->add_component<coopa::gfx::engine::components::DirectionalLightComponent>();
    light->direction = glm::vec3(0.0f, 0.0f, -1.0f);
    light->intensity = 2.5f;
    scene.add_root_object(std::move(obj));
    toy::weather::WeatherSystem* w = toy::weather::install_weather_system(scene, weather_test_block("time_of_day: 0.5\n"));
    weather_step(*w, scene, 0.2f);
    expect(w->root() == nullptr, "weather: an authored sun is driven, no runtime one made");
    expect(light->direction.z > 0.0f || light->intensity < 0.5f, "weather: at night the light is the dim moon");
    toy::weather::Settings s = w->settings();
    s.drive_sun = false;
    w->set_settings(s);
    weather_step(*w, scene, 0.1f);
    expect(light->direction == glm::vec3(0.0f, 0.0f, -1.0f) && light->intensity == 2.5f, "weather: letting go restores the authored light");
}

COOPA_TEST(schedule_and_clock_signals_fire) {
    coopa::scene::Scene scene("weather");
    // Day length 0.24 min: one game hour per 0.6 s.
    toy::weather::WeatherSystem* w = toy::weather::install_weather_system(
        scene, weather_test_block("schedule: random\nday_length_minutes: 0.24\ntime_of_day: 23.5\n"));
    int hours = 0, days = 0, changes = 0;
    bool saw_never = false;
    auto c1 = w->on_hour.connect_scoped([&](int) { ++hours; });
    auto c2 = w->on_new_day.connect_scoped([&](int) { ++days; });
    auto c3 = w->on_condition_changed.connect_scoped([&](const std::string&, const std::string& to) { ++changes; saw_never |= to == "never"; });
    // Durations are 6 s, transitions 10 s: a change every ~16 s.
    weather_step(*w, scene, 60.0f);
    expect(days == 5 && w->state().day == 5, "weather clock: 100 game hours from 23:30 cross midnight five times (" + std::to_string(days) + ")");
    expect(hours >= 99 && hours <= 101, "weather clock: on_hour fires once per game hour (" + std::to_string(hours) + ")");
    expect(changes >= 2, "weather schedule: random moves on when a duration runs out (" + std::to_string(changes) + ")");
    expect(!saw_never, "weather schedule: weight 0 is never picked");

    // Edit mode holds the clock unless the editor previews.
    scene.set_simulating(false);
    const float held = w->time_of_day();
    weather_step(*w, scene, 3.0f);
    expect_near(w->time_of_day(), held, 1e-5f, "weather clock: stands still in edit mode");
    w->set_editor_preview(true);
    weather_step(*w, scene, 3.0f);
    expect(std::abs(w->time_of_day() - held) > 1.0f, "weather clock: runs in edit mode with the editor preview on");

    // Cycle goes down the list.
    coopa::scene::Scene s2("cycle");
    toy::weather::WeatherSystem* c = toy::weather::install_weather_system(s2, weather_test_block("schedule: cycle\n"));
    std::vector<std::string> seen;
    auto c4 = c->on_condition_changed.connect_scoped([&](const std::string&, const std::string& to) { seen.push_back(to); });
    weather_step(*c, s2, 40.0f);
    expect(seen.size() >= 2 && seen[0] == "wet" && seen[1] == "never", "weather schedule: cycle follows the list order");
}

COOPA_TEST(reactor_rules_match_hours_phase_condition_and_rain) {
    using namespace toy::weather;
    WeatherReactor r;
    WeatherState w;
    w.hour = 20.0f; w.phase = DayPhase::Night; w.condition = "rain"; w.precipitation = 0.6f;
    r.use_hours = true; r.from_hour = 18.0f; r.to_hour = 6.0f;
    expect(r.matches(w), "reactor: hours wrap past midnight (20:00 in 18-6)");
    w.hour = 12.0f;
    expect(!r.matches(w), "reactor: ...noon is outside");
    r.use_hours = false;
    r.phases = {"night"};
    expect(!r.matches([&] { WeatherState d = w; d.phase = DayPhase::Day; return d; }()), "reactor: phase rule");
    r.conditions = {"rain", "storm"};
    expect(r.matches(w), "reactor: condition rule");
    r.min_precipitation = 0.8f;
    expect(!r.matches(w), "reactor: precipitation rule");
}
