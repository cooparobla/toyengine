/**
 * @file hair_test.cpp
 * @brief The hair scene end to end: grooms grow from their meshes, reach the screen, sway in the
 *        wind at no per-frame cost, and the one simulated groom is actually solved.
 *
 * The unit suite (`hair_data`) covers every decision about where a strand goes, and physxcoopa's
 * `hair_solver` covers the solve. What neither can catch is the plumbing between them and the
 * screen: a groom that never published its mesh, a surface shader that never ran, a dynamic
 * vertex buffer uploaded once and then frozen, or guides that were never registered with the
 * physics world. Each of those draws a perfectly plausible frame while being wrong.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/hair/hair.h>
#include <toyengine/hair/hair_system.h>
#include <physxcoopa/hair/strand.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/runtime_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("hair");

/// How far a mid-strand particle's offset from its own root must vary over the tail's swing.
///
/// Both ends measured in THIS scene: 0.353 m with the authored loose settings, 0.005 m with
/// `sim_stiffness` pinned to 1.0. The floor is not zero because collision against the ball moves
/// particles whatever the stiffness. 0.1 sits between the two with 3x headroom either way, and
/// the failure message reports what was actually measured so a drift is diagnosable from the log.
constexpr float k_flex_threshold = 0.1f;

using namespace toy::test;

namespace {

/// The runtime child HairSystem publishes under `owner`, found by its marker rather than by a
/// remembered pointer -- the same way the system itself finds it.
coopa::scene::SceneObject* hair_child(coopa::scene::SceneObject* owner) {
    if (owner == nullptr) return nullptr;
    for (const auto& child : owner->children()) {
        if (child == nullptr || child->name() != toy::hair::Hair::child_object_name()) continue;
        const auto* marker = child->get_component<toy::scene::RuntimeObject>();
        if (marker != nullptr && marker->owner_system == "Hair") return child.get();
    }
    return nullptr;
}

} // namespace

/**
 * @brief Every groom in the demo scene grows, publishes a locked runtime child, and draws.
 *
 * The published child is the part that is easy to get silently wrong: without its RuntimeObject
 * marker the editor would write thousands of strands into the scene file on every save.
 */
COOPA_TEST(every_groom_grows_and_publishes_a_locked_runtime_child) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/effects/hair_demo/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 10);

    auto* system = dynamic_cast<toy::hair::HairSystem*>(engine.scene().find_system("Hair"));
    expect(system != nullptr, "hair scene: HairSystem is installed");

    const std::vector<toy::hair::Hair*> grooms = engine.scene().get_components<toy::hair::Hair>();
    expect(grooms.size() == 4u, "hair scene: four Hair components parsed from YAML");

    for (toy::hair::Hair* hair : grooms) {
        const std::string name = hair->owner != nullptr ? hair->owner->name() : std::string("?");
        expect(system != nullptr && system->strand_count(name) > 0u,
               "hair scene: '" + name + "' grew strands from its mesh");
        expect(system != nullptr && system->vertex_count(name) > 0u,
               "hair scene: '" + name + "' built a mesh and uploaded it");

        coopa::scene::SceneObject* child = hair_child(hair->owner);
        expect(child != nullptr,
               "hair scene: '" + name + "' published a RuntimeObject-marked child -- without the "
               "marker every save would bake its strands into the scene file");
        const auto* mr = child != nullptr
                             ? child->get_component<coopa::gfx::engine::components::MeshRenderer>()
                             : nullptr;
        expect(mr != nullptr && mr->is_ready(),
               "hair scene: and that child is drawing the groom");
        expect(mr == nullptr || mr->material.shader == "hair",
               "hair scene: through the `hair` surface shader");
    }
}

/** @brief `density` is per square metre, so the same number means more hair on a bigger scalp. */
COOPA_TEST(the_tube_groom_is_a_handful_of_strands_and_the_fine_one_is_thousands) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/effects/hair_demo/scene.yaml", 320, 180, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 10);

    auto* system = dynamic_cast<toy::hair::HairSystem*>(engine.scene().find_system("Hair"));
    expect(system != nullptr, "hair scene: HairSystem is installed");
    if (system == nullptr) return;

    const std::size_t fine = system->strand_count("fine_hair");
    const std::size_t chunky = system->strand_count("chunky_hair");
    expect(fine > 1000u, "hair scene: the fine groom is thousands of strands");
    expect(chunky > 2u && chunky < 60u,
           "hair scene: and the chunky one is a handful of thick tubes -- the same system, the "
           "same keys, a different thickness and density");
}

/**
 * @brief A static groom costs nothing per frame and still moves: the sway is in the shader.
 *
 * This is the claim the whole default path rests on. A groom that moved by being re-uploaded
 * every frame would look identical and cost thousands of times more.
 */
COOPA_TEST(a_static_groom_sways_in_the_wind_without_being_re_uploaded) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/effects/hair_demo/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    auto* system = dynamic_cast<toy::hair::HairSystem*>(engine.scene().find_system("Hair"));
    expect(system != nullptr, "hair scene: HairSystem is installed");
    if (system == nullptr) return;

    // Not simulated, so no guides are registered with the physics world at all.
    expect(system->simulated_guide_count("fine_hair") == 0u,
           "hair scene: the fine groom is static -- it solves nothing");
    expect(system->simulated_guide_count("fur_box") == 0u,
           "hair scene: so is the fur");

    const Frame early = engine.capture_image(/*low_res=*/true);
    tick_frames(engine, 60);
    const Frame late = engine.capture_image(true);

    const long long moved = count_diff(early, late, 6);
    const long long total = static_cast<long long>(early.width) * early.height;
    expect(moved * 100 > total,
           "hair scene: the image changes between frames -- the `hair` shader's vertex hook is "
           "running and the scene's breeze is driving it (it returns early in dead air)");
}

/** @brief The one simulated groom really is handed to the solver, and really is solved. */
COOPA_TEST(the_simulated_groom_registers_guides_and_they_move) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/effects/hair_demo/scene.yaml", 320, 180, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 20);

    auto* system = dynamic_cast<toy::hair::HairSystem*>(engine.scene().find_system("Hair"));
    expect(system != nullptr, "hair scene: HairSystem is installed");
    if (system == nullptr) return;

    const std::size_t guides = system->simulated_guide_count("swinging_tail");
    expect(guides > 0u,
           "hair scene: the simulated groom registered guide strands with the physics world");
    expect(guides <= 48u,
           "hair scene: and only `sim_guides` of them -- the solver's cost scales with this, not "
           "with how much hair is drawn");
    expect(guides < system->strand_count("swinging_tail"),
           "hair scene: far fewer guides than render strands, which is the whole point");

    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(
        engine.scene().find_system("Physics"));
    expect(physics != nullptr, "hair scene: the scene has physics to solve them");
    if (physics == nullptr) return;
    expect(physics->world().strand_set_count() == 1u,
           "hair scene: exactly one strand set is alive -- one simulated groom, one set");

    auto* tail = engine.scene().find_object("swinging_tail");
    expect(tail != nullptr, "hair scene: the swinging tail resolves by name");
    // The hair must move RELATIVE TO ITS OWN ROOT. Pixels changing, or world positions changing,
    // would both pass just as happily on hair welded rigidly to a moving ball, so this measures
    // an offset from the root and asks whether that offset itself varies.
    //
    // A MID-strand particle, not the tip: `sim_stiffness_falloff` deliberately frees the tip
    // entirely -- its weight is (1 - 1)^falloff = 0 -- so a tip swings even on a groom pinned as
    // rigid as the solver allows. The midpoint is where stiffness actually shows.
    const auto mid_offset = [&]() {
        glm::vec3 offset(0.0f);
        physics->world().for_each_strands([&](coopa::physx::hair::StrandSetId,
                                              const coopa::physx::hair::StrandSet& set) {
            if (set.strands.empty()) return;
            const auto& strand = set.strands[0];
            if (strand.count < 3) return;
            offset = set.particles[strand.first + strand.count / 2].position -
                     set.particles[strand.first].position;
        });
        return offset;
    };

    float widest = 0.0f;
    const glm::vec3 first = mid_offset();
    for (int i = 0; i < 12; ++i) {
        tick_frames(engine, 10);
        widest = std::max(widest, glm::length(mid_offset() - first));
    }
    expect(widest > k_flex_threshold,
           "hair scene: the strand really flexes as the body swings (" + std::to_string(widest) +
               " m, needs > " + std::to_string(k_flex_threshold) +
               ") -- hair welded rigidly to the ball would cross the screen just as far and tell "
               "us nothing");

    // And it must reach the screen: a simulated groom is re-uploaded every frame from the
    // solver, and a frozen vertex buffer would draw hair that ignores the body it hangs on.
    const Frame a = engine.capture_image(true);
    tick_frames(engine, 20);
    const Frame b = engine.capture_image(true);
    expect(count_diff(a, b, 4) > 0,
           "hair scene: the simulated groom's drawn pixels keep changing as its body swings");
}
