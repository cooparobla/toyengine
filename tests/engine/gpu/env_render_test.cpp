/**
 * @file env_render_test.cpp
 * @brief environment_demo end to end: the scene's local influences reach the surface world UBO
 *        with the shape and channels the CPU field holds, the CPU and GPU folds agree (the
 *        "keep the two in step" rule between env_field.h, world.glsl and surface_world.cpp),
 *        the helicopter's downwash actually presses the grass it passes over, and a scene that
 *        authors no influence packs none -- so every existing scene is unaffected.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <glm/glm.hpp>

#include <toyengine/core/engine.h>
#include <uicoopa/binding/ui_handle.h>
#include <uicoopa/widgets/button.h>
#include <uicoopa/builder/theme_scope.h>
#include <coopa/animation/animator.h>

#include <toyengine/env/env_preset.h>
#include <toyengine/env/env_system.h>
#include <toyengine/render/toy_render_pipeline.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/weather_fixtures.h"

COOPA_TEST_SUITE("env_render");

using namespace toy::test;

namespace {

const char* k_env_demo = "assets/scenes/effects/environment_demo/scene.yaml";

/** @brief Mirrors world.glsl's gfx_env_falloff() against a packed UBO source. */
float gpu_falloff(const toy::render::EnvSourceGpu& g, glm::vec3 p) {
    const float radius = g.pos_radius.w;
    if (radius <= 0.0f) return 0.0f;
    const glm::vec3 d = p - glm::vec3(g.pos_radius);
    const int flags = static_cast<int>(g.params.z);
    float r;
    if ((flags & toy::render::kEnvShapeDisc) != 0) {
        const float height = g.params.y;
        if (height > 0.0f && std::abs(d.z) > height) return 0.0f;
        r = std::sqrt(d.x * d.x + d.y * d.y);
    } else {
        r = std::sqrt(glm::dot(d, d));
    }
    if (r >= radius) return 0.0f;
    const float inner = radius * (1.0f - std::clamp(g.params.x, 0.0f, 1.0f));
    if (r <= inner) return 1.0f;
    const float t = (radius - r) / std::max(radius - inner, 1e-6f);
    return t * t * (3.0f - 2.0f * t);
}

/** @brief Mirrors world.glsl's gfx_env_wind_full() against a packed UBO source. */
glm::vec3 gpu_wind_full(const toy::render::EnvSourceGpu& g, glm::vec3 p) {
    const int flags = static_cast<int>(g.params.z);
    if ((flags & toy::render::kEnvModeRadial) == 0) return glm::vec3(g.wind);
    glm::vec2 radial(p.x - g.pos_radius.x, p.y - g.pos_radius.y);
    const float len = std::sqrt(glm::dot(radial, radial));
    const glm::vec2 dir = len > 1e-4f ? radial / len : glm::vec2(0.0f);
    return glm::vec3(dir * g.wind.x, g.wind.y);
}

} // namespace

COOPA_TEST(the_scenes_influences_reach_the_surface_world_ubo) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_env_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);
    // The scene starts on Calm, which has no local source by design -- pick one to pack.
    if (auto* sw = engine.scene().find_first_component<toy::env::EnvPresetSwitcher>()) sw->select_by_name("rotor");
    tick_frames(engine, 60);

    const toy::env::EnvSystem* es = toy::env::find_env(engine.scene());
    require(es != nullptr, "env render: the scene has an EnvSystem");
    const toy::env::EnvField& f = es->field();
    expect(f.count > 0, "env render: the demo's influences were gathered (" + std::to_string(f.count) + ")");

    const auto& u = engine.pipeline().surface_world_ubo();
    require(u.terrain_count.y == f.count,
            "env render: the UBO carries exactly the field's source count -- this is the "
            "CPU/GPU packing contract the three folds share");

    // Every packed source must match its CPU twin, field by field.
    for (int i = 0; i < f.count; ++i) {
        const toy::env::EnvSource& c = f.source[i];
        const toy::render::EnvSourceGpu& g = u.env[i];
        expect_near(g.pos_radius.x, c.position.x, 1e-4f, "env render: packed position x");
        expect_near(g.pos_radius.y, c.position.y, 1e-4f, "env render: packed position y");
        expect_near(g.pos_radius.z, c.position.z, 1e-4f, "env render: packed position z");
        expect_near(g.pos_radius.w, c.radius, 1e-4f, "env render: packed radius");
        expect_near(g.params.x, c.falloff, 1e-4f, "env render: packed falloff");
        expect_near(g.params.w, c.wetness, 1e-4f, "env render: packed wetness");
        const int flags = static_cast<int>(g.params.z);
        expect(((flags & toy::render::kEnvShapeDisc) != 0) == (c.shape == toy::env::EnvShape::Disc),
               "env render: packed shape flag");
        expect(((flags & toy::render::kEnvModeRadial) != 0) == (c.mode == toy::env::EnvMode::Radial),
               "env render: packed mode flag");
        expect(((flags & toy::render::kEnvBlendOverride) != 0) == (c.blend == toy::env::EnvBlend::Override),
               "env render: packed override flag -- the shelter station depends on this");
    }
}

COOPA_TEST(the_cpu_and_gpu_folds_agree) {
    // The standing rule in wind.h / world.glsl / env_field.h is "keep these in step". Nothing
    // but a test like this notices when they drift, and the symptom would be grass that bends
    // differently from the cloth beside it.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_env_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);
    if (auto* sw = engine.scene().find_first_component<toy::env::EnvPresetSwitcher>()) sw->select_by_name("rotor");
    tick_frames(engine, 60);

    const toy::env::EnvSystem* es = toy::env::find_env(engine.scene());
    require(es != nullptr, "env fold: the system is installed");
    const toy::env::EnvField& f = es->field();
    const auto& u = engine.pipeline().surface_world_ubo();
    require(u.terrain_count.y == f.count, "env fold: counts match");
    require(f.count > 0, "env fold: there is something to compare");

    int compared = 0;
    float worst = 0.0f;
    // Sample a grid over the whole apron, at grass height and above it.
    for (float x = -50.0f; x <= 60.0f; x += 2.0f) {
        for (float y = -12.0f; y <= 12.0f; y += 3.0f) {
            for (float z : {0.3f, 4.0f}) {
                const glm::vec3 p(x, y, z);
                for (int i = 0; i < f.count; ++i) {
                    const float cpu_w = toy::env::env_falloff(f.source[i], p);
                    const float gpu_w = gpu_falloff(u.env[i], p);
                    worst = std::max(worst, std::abs(cpu_w - gpu_w));
                    if (cpu_w > 0.0f) {
                        const glm::vec3 cpu_v = toy::env::env_wind_full(f.source[i], p);
                        const glm::vec3 gpu_v = gpu_wind_full(u.env[i], p);
                        worst = std::max(worst, glm::length(cpu_v - gpu_v));
                    }
                    ++compared;
                }
            }
        }
    }
    expect(compared > 500, "env fold: the grid actually covered the sources (" + std::to_string(compared) + " samples)");
    expect(worst < 1e-4f,
           "env fold: the CPU and the packed-GPU falloff and wind agree everywhere (worst "
           + std::to_string(worst) + ")");
}

COOPA_TEST(each_preset_puts_its_own_environment_in_the_field) {
    // The scene's whole contract: the bar picks one environment, and picking it changes what
    // the field says at the clearing's centre. Driven through EnvPresetSwitcher directly --
    // clicking a button is the same call with a UI event in front of it.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_env_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    auto* sw = engine.scene().find_first_component<toy::env::EnvPresetSwitcher>();
    require(sw != nullptr, "presets: the scene has an EnvPresetSwitcher");
    const toy::env::EnvSystem* es = toy::env::find_env(engine.scene());
    require(es != nullptr, "presets: the scene has an EnvSystem");

    const glm::vec3 over(2.5f, 0.0f, 0.4f);   // off-axis, at grass height, inside every source

    // Calm: the scene starts here. No local source at all.
    expect(sw->selected() == 0, "presets: it starts on the first preset");
    expect(es->field().count == 0,
           "presets: Calm has no local source -- the baseline really is the bare weather");
    const float calm_speed = glm::length(toy::env::wind_at(es->field(), over));

    // Rotor: a downwash. Pressing DOWN is the part the foliage shader turns into flattening.
    require(sw->select_by_name("rotor"), "presets: 'rotor' exists");
    tick_frames(engine, 90);
    expect(es->field().count > 0, "presets: Rotor puts a source in the field");
    const glm::vec3 wash = toy::env::wind_at(es->field(), over);
    expect(wash.z < -3.0f,
           "presets: the rotor's wash presses DOWN at grass height (" + std::to_string(wash.z) + " m/s)");
    expect(wash.x > 1.0f, "presets: ...and fans outward from the axis");

    // Vent: the same radial source with the sign flipped. One component, both behaviours.
    require(sw->select_by_name("vent"), "presets: 'vent' exists");
    tick_frames(engine, 30);
    const glm::vec3 up = toy::env::wind_at(es->field(), over);
    expect(up.z > 3.0f,
           "presets: the vent LIFTS where the rotor pressed (" + std::to_string(up.z) + " m/s)");

    // Gale: no local source; the GLOBAL weather does the work. The weather blends, so give it
    // time to arrive.
    require(sw->select_by_name("gale"), "presets: 'gale' exists");
    tick_frames(engine, 420);
    expect(es->field().count == 0, "presets: Gale adds no local source");
    const float gale_speed = glm::length(toy::env::wind_at(es->field(), over));
    expect(gale_speed > calm_speed + 3.0f,
           "presets: Gale really is windier than Calm (" + std::to_string(calm_speed) + " -> " +
           std::to_string(gale_speed) + " m/s) -- the preset moved the weather, not a local source");

    // Shelter: the same gale, plus an override. Inside the stones the air must be still AGAIN,
    // which is the one thing `override` exists for.
    require(sw->select_by_name("shelter"), "presets: 'shelter' exists");
    tick_frames(engine, 60);
    expect(es->field().count > 0, "presets: Shelter puts its override source in the field");
    const float inside = glm::length(toy::env::wind_at(es->field(), glm::vec3(0.0f, 0.0f, 0.6f)));
    const float outside = glm::length(toy::env::wind_at(es->field(), glm::vec3(26.0f, 0.0f, 0.6f)));
    expect(inside < 1.0f,
           "presets: inside the stones the gale does not reach (" + std::to_string(inside) + " m/s)");
    expect(outside > gale_speed * 0.5f,
           "presets: ...while the meadow outside still streams (" + std::to_string(outside) + " m/s)");

    // Back to Calm: switching away must take the source with it, or presets would accumulate.
    require(sw->select_by_name("calm"), "presets: 'calm' exists");
    tick_frames(engine, 30);
    expect(es->field().count == 0,
           "presets: switching back to Calm deactivates the previous preset's subtree -- the "
           "gather skips an EnvInfluence whose object is inactive");
}

COOPA_TEST(a_button_click_switches_the_preset_and_replays_it) {
    // What the panel actually promises, and what no test covered before: the bar's buttons
    // exist under the names the switcher listens for, a click on one switches, and switching
    // REPLAYS the preset rather than resuming it mid-cycle.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_env_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    auto* sw = engine.scene().find_first_component<toy::env::EnvPresetSwitcher>();
    require(sw != nullptr, "buttons: the switcher is in the scene");

    // 1. Every button the switcher listens for was actually built by the panel, under that
    //    exact name -- the name IS the event-bus channel, so a typo silently does nothing.
    for (const auto& preset : sw->presets) {
        if (preset.button.empty()) continue;
        coopa::scene::SceneObject* o = engine.scene().find_object(preset.button);
        expect(o != nullptr, "buttons: the panel built '" + preset.button + "'");
        if (o) {
            expect(coopa::ui::UiHandle(o).find<coopa::ui::Button>(preset.button) != nullptr,
                   "buttons: '" + preset.button + "' carries a Button");
        }
    }

    // 2. A click switches. This is exactly what Button::on_pointer_click publishes.
    engine.scene().events().emit("BtnVent", "click");
    tick_frames(engine, 2);
    expect(sw->selected() >= 0 && sw->presets[static_cast<std::size_t>(sw->selected())].name == "vent",
           "buttons: clicking BtnVent selects the vent preset");

    // 3. Exactly one preset subtree is live at a time.
    int live = 0;
    for (const auto& preset : sw->presets) {
        if (preset.object.empty()) continue;
        if (coopa::scene::SceneObject* o = engine.scene().find_object(preset.object)) {
            if (o->active()) ++live;
        }
    }
    expect(live == 1, "buttons: exactly one preset subtree is active (" + std::to_string(live) + ")");

    // No button may LOOK selected except the live one. The panel shipped with two buttons
    // stuck in the theme's Primary role, which reads as two active presets at once.
    int tinted = 0;
    for (const auto& preset : sw->presets) {
        if (preset.button.empty()) continue;
        coopa::scene::SceneObject* o = engine.scene().find_object(preset.button);
        if (!o) continue;
        if (auto* b = coopa::ui::UiHandle(o).find<coopa::ui::Button>(preset.button)) {
            if (b->colors.normal == coopa::ui::theme_for(o).button_primary.normal) ++tinted;
        }
    }
    expect(tinted == 1,
           "buttons: exactly one button is highlighted (" + std::to_string(tinted) + ") -- a "
           "button's authored style must never imply selection");

    // 4. A preset authored `active: false` has never had its components started -- an inactive
    //    object is skipped by SceneObject::start() and set_active() does not run it later. The
    //    switcher must start it on first activation, or the preset is inert.
    engine.scene().events().emit("BtnSwing", "click");
    tick_frames(engine, 20);
    coopa::scene::SceneObject* swing = engine.scene().find_object("preset_swing");
    require(swing != nullptr, "buttons: the swing preset exists");
    auto* animator = swing->get_component<coopa::anim::Animator>();
    require(animator != nullptr, "buttons: it has an Animator");
    expect(animator->is_playing(),
           "buttons: activating a preset STARTS it -- an authored-off subtree is never started "
           "by the scene, so without this its animation never runs");
    const float advanced = animator->time();
    expect(advanced > 0.0f, "buttons: and the clip is advancing");

    // 5. Switching away and back REPLAYS from the beginning.
    engine.scene().events().emit("BtnCalm", "click");
    tick_frames(engine, 10);
    engine.scene().events().emit("BtnSwing", "click");
    tick_frames(engine, 1);
    expect(animator->time() < advanced,
           "buttons: re-selecting a preset replays it from the start rather than resuming where "
           "it was left (" + std::to_string(animator->time()) + " vs " + std::to_string(advanced) + ")");

    // Clicking the preset that is ALREADY live replays it too -- the button is a "play again"
    // control, not a dead one, so it must not be disabled or early-outed.
    tick_frames(engine, 20);
    const float before_reclick = animator->time();
    expect(before_reclick > 0.0f, "buttons: the clip advanced before the re-click");
    engine.scene().events().emit("BtnSwing", "click");
    tick_frames(engine, 1);
    expect(animator->time() < before_reclick,
           "buttons: clicking the LIVE preset replays it (" + std::to_string(animator->time()) +
           " vs " + std::to_string(before_reclick) + ")");
}

COOPA_TEST(a_scene_with_no_influences_packs_none) {
    // The no-regression guard: every scene that predates this feature must pack zero sources,
    // so each GPU fold early-outs and costs nothing.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/effects/weather_demo/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 40);

    expect(engine.pipeline().surface_world_ubo().terrain_count.y == 0,
           "env render: weather_demo authors no EnvInfluence, so the UBO packs no sources and "
           "every gfx_env_* fold early-outs");
    const toy::env::EnvSystem* es = toy::env::find_env(engine.scene());
    expect(es && es->field().count == 0, "env render: ...and the field itself is source-free");
}
