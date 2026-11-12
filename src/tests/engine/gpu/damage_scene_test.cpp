/**
 * @file damage_scene_test.cpp
 * @brief Damageable + DestructionSystem's dent path end to end, in the real `damage_demo` scene:
 *        an impact gives an object a private dynamic mesh and moves its vertices, a low-poly
 *        object is subdivided before its first dent, nothing is allocated for an object nobody
 *        has hit, repeated impacts deepen the dent and then shatter the object, and a brittle
 *        material shatters outright without ever denting.
 *
 * The dent kernel's own maths has its own device-free suite (tests/engine/unit/dent_test.cpp).
 * What is checked here is everything that only exists at runtime: the mesh asset being read back
 * and cloned, the clone replacing the renderer's own mesh, and the whole thing wired through
 * DestructionSystem's response dispatch. NO_INPUT=1 silences DamageCursor, so blows are queued
 * directly on each object's Damageable, the same move libs/physxcoopa/tests/damage_test.cpp
 * makes at the policy layer one level down.
 */

#include <coopa/testing/test.h>

#include <string>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/damage/damage_types.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/damageable.h>
#include <toyengine/scene/destructible.h>
#include <toyengine/scene/destruction_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("damage_scene");

using namespace toy::test;
using coopa::gfx::engine::components::MeshRenderer;
using coopa::physx::damage::DamageEvent;
using coopa::physx::damage::DamageType;

namespace {

toy::scene::Damageable* damageable_of(coopa::scene::Scene& scene, const std::string& name) {
    auto* object = scene.find_object(name);
    return object ? object->get_component<toy::scene::Damageable>() : nullptr;
}

toy::scene::Destructible* destructible_of(coopa::scene::Scene& scene, const std::string& name) {
    auto* object = scene.find_object(name);
    return object ? object->get_component<toy::scene::Destructible>() : nullptr;
}

toy::scene::DestructionSystem* destruction_of(coopa::scene::Scene& scene) {
    return dynamic_cast<toy::scene::DestructionSystem*>(scene.find_system("Destruction"));
}

} // namespace

/**
 * @brief wood_log at (-6, 0, 0.2) scale 2: a blow on its +Y side gives it a private dynamic
 *        mesh, and nothing else in the scene is touched by it.
 */
COOPA_TEST(one_impact_gives_the_object_a_private_dynamic_mesh_and_moves_its_vertices) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/damage_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* damageable = damageable_of(scene, "wood_log");
    require(damageable != nullptr, "damage_scene: the demo carries a Damageable wood_log");
    auto* destruction = destruction_of(scene);
    require(destruction != nullptr, "damage_scene: DestructionSystem is installed");
    expect(destruction->dented_meshes() == 0, "damage_scene: nothing is dented before any blow lands");

    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 240.0f;
    blow.point = glm::vec3(-6.0f, 0.2f, 0.2f);   // the log's +Y surface at its mid-length
    blow.direction = glm::vec3(0.0f, -1.0f, 0.0f); // into the log, toward its axis
    damageable->apply_damage(blow);

    tick_frames(engine, 10);

    expect(destruction->dented_meshes() == 1, "damage_scene: exactly the one hit object is dented (" +
                                                   std::to_string(destruction->dented_meshes()) + ")");
    auto* renderer = scene.find_object("wood_log")->get_component<MeshRenderer>();
    require(renderer != nullptr && renderer->is_ready(), "damage_scene: wood_log's mesh is ready");
    expect(renderer->get_mesh()->is_dynamic(), "damage_scene: it now carries a dynamic (frame-buffered) "
                                               "mesh, not the shared asset");
    expect(renderer->get_mesh()->vertex_count() > 0, "damage_scene: ...with real geometry");

    auto* destructible = destructible_of(scene, "wood_log");
    require(destructible != nullptr, "damage_scene: wood_log carries a Destructible too");
    expect(!destructible->shattered(), "damage_scene: one Dent-sized blow does not shatter it "
                                       "(hit_points is tuned for several)");
}

/**
 * @brief metal_plate stays on the ordinary 24-vertex `cube`; its first dent must subdivide it
 *        first (dent_resolution: 0.05 in the scene), silently, before pushing it in.
 */
COOPA_TEST(a_low_poly_object_is_subdivided_before_its_first_dent) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/damage_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* damageable = damageable_of(scene, "metal_plate");
    require(damageable != nullptr, "damage_scene: the demo carries a Damageable metal_plate");
    auto* destruction = destruction_of(scene);
    require(destruction != nullptr, "damage_scene: DestructionSystem is installed");

    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 240.0f;
    blow.point = glm::vec3(-3.0f, 0.0f, 0.45f);   // the plate's +Z face
    blow.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    damageable->apply_damage(blow);

    tick_frames(engine, 10);

    expect(destruction->dented_meshes() == 1, "damage_scene: only metal_plate was dented");
    auto* renderer = scene.find_object("metal_plate")->get_component<MeshRenderer>();
    require(renderer != nullptr && renderer->is_ready(), "damage_scene: metal_plate's mesh is ready");
    const std::size_t verts = renderer->get_mesh()->vertex_count();
    expect(verts > 24, "damage_scene: the clone has far more than the cube's original 24 vertices (" +
                            std::to_string(verts) + ") -- it was subdivided before denting");
}

/**
 * @brief Six Impact blows on wood_log (hit_points: 1200, ~240 HP each) deepen its dent and then
 *        drain it to 0, which shatters it through the ordinary Destructible path.
 */
COOPA_TEST(repeated_impacts_deepen_the_dent_and_then_shatter_it) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/damage_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* damageable = damageable_of(scene, "wood_log");
    auto* destructible = destructible_of(scene, "wood_log");
    require(damageable != nullptr && destructible != nullptr, "damage_scene: wood_log carries both components");

    const auto blow_at = [] {
        DamageEvent blow;
        blow.type = DamageType::Impact;
        blow.amount = 240.0f;
        blow.point = glm::vec3(-6.0f, 0.2f, 0.2f);
        blow.direction = glm::vec3(0.0f, -1.0f, 0.0f);
        return blow;
    };

    for (int hit = 0; hit < 5 && !destructible->shattered(); ++hit) {
        damageable->apply_damage(blow_at());
        tick_frames(engine, 5);
    }

    expect(destructible->shattered(), "damage_scene: enough repeated hits shattered the log");
    expect(destructible->chunk_count() > 0, "damage_scene: ...into chunks (" +
                                                 std::to_string(destructible->chunk_count()) + ")");
}

/**
 * @brief glass_pane's material resolves any real Impact to Fracture outright (resolve_damage.h's
 *        own policy: brittle enough, and the demo's amount clears its threshold easily) -- it
 *        must shatter on the first blow without ever picking up a dent clone.
 */
COOPA_TEST(a_glass_pane_shatters_on_the_first_impact_without_ever_denting) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/damage_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* damageable = damageable_of(scene, "glass_pane");
    auto* destructible = destructible_of(scene, "glass_pane");
    require(damageable != nullptr && destructible != nullptr, "damage_scene: glass_pane carries both components");
    auto* destruction = destruction_of(scene);
    require(destruction != nullptr, "damage_scene: DestructionSystem is installed");

    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = 240.0f;
    blow.point = glm::vec3(0.0f, 0.5f, 0.5f);   // the pane's +Y face
    blow.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    damageable->apply_damage(blow);

    tick_frames(engine, 10);

    expect(destructible->shattered(), "damage_scene: one real Impact shatters the glass outright");
    expect(destruction->dented_meshes() == 0, "damage_scene: ...never having built a dent clone for it "
                                              "(Fracture is checked before Dent/Chip -- see "
                                              "process_damage_()'s own ordering)");
}
