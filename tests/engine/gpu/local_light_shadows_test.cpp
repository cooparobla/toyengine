/**
 * @file local_light_shadows_test.cpp
 * @brief Point and spot shadows from the local-light atlas reach the image, obey the per-type
 *        budgets, and belong to the light that casts them (a past slot-ownership bug).
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <memory>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <coopa/scene/scene_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("local_light_shadows");

using namespace toy::test;

/**
 * @brief Point and spot shadows come from the local-light atlas: they reach the image, obey the
 *        per-type budgets, and belong to the light that casts them.
 *
 * pixel_demo authors one point and one spot light, both cast_shadows and both inactive; this
 * switches them on and reads the `direct` channel (direct light only, shadows applied, no fog or
 * post), comparing:
 *   - with vs without the lights' cast_shadows: the local shadows must change the image;
 *   - budgets of 0 point / 0 spot shadows vs cast_shadows off: identical images;
 *   - shadow-slot ownership: a non-casting light listed first must not take the shadow slot
 *     from the real caster (a renderer that shadows the FIRST cast_shadows light but flags
 *     point_lights[0] as its owner leaves the real caster unshadowed). Here the authored light stops casting and
 *     loses its intensity, and an identical casting light is appended AFTER it: the image must
 *     match the authored light's shadowed image again.
 */
COOPA_TEST(shadows_reach_the_image_obey_budgets_and_keep_their_owner) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    auto* point_obj = engine.scene().find_object("point");
    auto* spot_obj  = engine.scene().find_object("spot");
    expect(point_obj != nullptr && spot_obj != nullptr, "local shadows: pixel_demo has its point and spot light");
    if (!point_obj || !spot_obj) return;
    point_obj->set_active(true);
    spot_obj->set_active(true);
    auto* pl = point_obj->get_component<coopa::gfx::engine::components::PointLightComponent>();
    auto* sl = spot_obj->get_component<coopa::gfx::engine::components::SpotLightComponent>();
    expect(pl != nullptr && sl != nullptr && pl->cast_shadows && sl->cast_shadows,
           "local shadows: both authored lights cast shadows");
    if (!pl || !sl) return;
    // pixel_demo's point light reaches 100 units (a glow effect); bring it in so its shadows
    // land on the floor around it with real contrast.
    pl->range = 12.0f;
    engine.render_config().debug_view = "direct";

    tick_frames(engine, kNoiseCycle);
    const Frame shadowed = engine.capture_image(true);

    pl->cast_shadows = false;
    sl->cast_shadows = false;
    tick_frames(engine, kNoiseCycle);
    const Frame unshadowed = engine.capture_image(true);
    const long long local_px = count_diff(shadowed, unshadowed, 2);
    expect_at_least(local_px, 200, "local shadows: point/spot shadows change the direct-light image");

    // Budgets: casting lights, but no slots -- the same image as not casting at all.
    pl->cast_shadows = true;
    sl->cast_shadows = true;
    engine.render_config().max_shadowed_point_lights = 0;
    engine.render_config().max_shadowed_spot_lights  = 0;
    tick_frames(engine, kNoiseCycle);
    const Frame no_budget = engine.capture_image(true);
    const long long budget_diff = count_diff(no_budget, unshadowed, 2);
    expect(budget_diff <= 4, "local shadows: zero point/spot budgets leave every local light unshadowed");
    if (budget_diff > 4) {
        std::cerr << "         zero-budget image differs from the no-shadow one in " << budget_diff << " px\n";
        dump_frame(no_budget, "local_shadows_no_budget");
        dump_frame(unshadowed, "local_shadows_unshadowed");
    }
    engine.render_config().max_shadowed_point_lights = 4;
    engine.render_config().max_shadowed_spot_lights  = 4;

    // The slot bug: a non-casting point light FIRST, the real caster after it.
    pl->cast_shadows = false;
    const float intensity = pl->intensity;
    pl->intensity = 0.0f;
    {
        auto obj = std::make_unique<coopa::scene::SceneObject>("point_after");
        auto* tc = obj->add_component<coopa::scene::TransformComponent>();
        tc->transform().set_position(point_obj->get_transform()->transform().position());
        auto* caster = obj->add_component<coopa::gfx::engine::components::PointLightComponent>();
        caster->color                 = pl->color;
        caster->intensity             = intensity;
        caster->range                 = pl->range;
        caster->cast_shadows          = true;
        caster->attenuation_constant  = pl->attenuation_constant;
        caster->attenuation_linear    = pl->attenuation_linear;
        caster->attenuation_quadratic = pl->attenuation_quadratic;
        engine.scene().add_root_object(std::move(obj));
    }
    tick_frames(engine, kNoiseCycle);
    const Frame second_caster = engine.capture_image(true);
    const long long vs_shadowed = count_diff(second_caster, shadowed, 2);
    expect(vs_shadowed <= local_px / 10,
           "local shadows: a casting point light listed after a non-casting one keeps its own shadow");
    if (local_px < 200 || vs_shadowed > local_px / 10) {
        std::cerr << "         local-shadow pixels " << local_px << ", second-caster diff " << vs_shadowed << "\n";
        dump_frame(shadowed, "local_shadows_shadowed");
        dump_frame(unshadowed, "local_shadows_unshadowed");
        dump_frame(second_caster, "local_shadows_second_caster");
    }
}
