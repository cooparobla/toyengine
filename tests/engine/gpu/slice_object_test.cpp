/**
 * @file slice_object_test.cpp
 * @brief DestructionSystem::slice_object() end to end: a plane cut spawns two rigid halves with
 *        a collider and a weighed Rigidbody, both rest on the ground, and a half with slices
 *        left can itself be cut again -- using the cached local-frame vertices
 *        Sliceable::cached_vertices carries, since a spawned half has no mesh asset file.
 *
 * The slice maths has its own device-free suite (tests/engine/unit/slice_test.cpp). What is
 * checked here is everything that only exists at runtime: the world-to-local plane transform,
 * the piece spawn (mesh, collider, Rigidbody), the original going away, and re-slicing a piece
 * that was itself spawned by a slice.
 */

#include <coopa/testing/test.h>

#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/convex_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/destructible.h>
#include <toyengine/scene/destruction_system.h>
#include <toyengine/scene/sliceable.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("slice_object");

using namespace toy::test;

namespace {

/** @brief Every piece DestructionSystem has spawned (fracture chunk or slice half) into `scene`. */
std::vector<coopa::scene::SceneObject*> chunks_of(coopa::scene::Scene& scene) {
    std::vector<coopa::scene::SceneObject*> out;
    for (const auto& object : scene.root_objects()) {
        if (object->name().rfind("chunk_", 0) == 0) out.push_back(object.get());
    }
    return out;
}

} // namespace

/**
 * @brief Slices the demo's crate in two, checks both halves are proper rigid pieces that settle
 *        on the ground, then slices one of the halves again -- the re-slicing path, which
 *        depends on the half's Sliceable carrying its own cached vertices.
 */
COOPA_TEST(slice_object_cuts_an_object_into_two_settled_halves_and_a_half_can_be_resliced) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/destruction_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();

    auto* crate_obj = scene.find_object("crate");
    require(crate_obj != nullptr, "slice_object: the demo scene carries a crate");
    auto* crate = crate_obj->get_component<toy::scene::Destructible>();
    require(crate != nullptr, "slice_object: ...with a Destructible");
    crate->max_slices = 2; // one slice now, one more on a half below

    auto* destruction = dynamic_cast<toy::scene::DestructionSystem*>(scene.find_system("Destruction"));
    require(destruction != nullptr, "slice_object: DestructionSystem is installed");

    // Called directly, synchronously -- nothing else runs between the before/after snapshot, so
    // the two new names are unambiguous even though the demo's OTHER weights are also about to
    // break things of their own once the scene starts ticking.
    const std::size_t before = chunks_of(scene).size();
    const int spawned = destruction->slice_object(scene, *crate_obj, glm::vec3(-8.0f, 0.0f, 1.0f),
                                                  glm::vec3(0.0f, 0.0f, 1.0f), 0.5f);
    expect(spawned == 2, "slice_object: a plane through the middle gives two halves");
    expect(!crate_obj->active(), "slice_object: the original is deactivated, not deleted");

    std::vector<coopa::scene::SceneObject*> all = chunks_of(scene);
    require(all.size() == before + 2, "slice_object: exactly two new pieces appeared (" +
                                           std::to_string(all.size() - before) + ")");
    std::vector<coopa::scene::SceneObject*> halves(all.end() - 2, all.end());

    int with_body = 0, with_collider = 0;
    float total_mass = 0.0f;
    for (coopa::scene::SceneObject* half : halves) {
        auto* body = half->get_component<coopa::physx::components::RigidbodyComponent>();
        if (body != nullptr) { ++with_body; total_mass += body->mass; }
        const bool hull = half->get_component<coopa::physx::components::ConvexCollider>() != nullptr;
        const bool boxed = half->get_component<coopa::physx::components::BoxCollider>() != nullptr;
        if (hull || boxed) ++with_collider;
    }
    expect(with_body == 2, "slice_object: both halves are rigid bodies");
    expect(with_collider == 2, "slice_object: ...with a collider");
    expect(total_mass > 0.0f, "slice_object: ...weighed from their own volume (" +
                                  std::to_string(total_mass) + " kg)");

    // One slice left: a half carries a Sliceable, decremented, with its own cached vertices
    // (it has no mesh asset file to re-read).
    auto* half_sliceable = halves.front()->get_component<toy::scene::Sliceable>();
    require(half_sliceable != nullptr, "slice_object: a half with slices left carries a Sliceable");
    expect(half_sliceable->max_slices == 1, "slice_object: ...decremented from the original's 2");
    expect(!half_sliceable->cached_vertices.empty(), "slice_object: ...with its own geometry cached");

    const glm::vec3 half_pos = halves.front()->get_transform()->transform().position();
    const int re_spawned = destruction->slice_object(scene, *halves.front(), half_pos,
                                                      glm::vec3(1.0f, 0.0f, 0.0f), 0.3f);
    expect(re_spawned == 2, "slice_object: a half with slices left can itself be cut");
    expect(!halves.front()->active(), "slice_object: ...and is itself deactivated once cut");

    // They fall, and the ground stops them: nothing below it, well after they have landed.
    tick_frames(engine, 150);
    int below_ground = 0;
    for (coopa::scene::SceneObject* chunk : chunks_of(scene)) {
        if (chunk->get_transform()->transform().position().z < -2.0f) ++below_ground;
    }
    expect(below_ground == 0, "slice_object: every piece rests on the ground rather than falling through");
}

/** @brief A plane that misses the mesh changes nothing: no pieces, object left alone. */
COOPA_TEST(slice_object_misses_leave_the_object_untouched) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/destruction_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* crate_obj = scene.find_object("crate");
    require(crate_obj != nullptr, "slice_object: the demo scene carries a crate");

    auto* destruction = dynamic_cast<toy::scene::DestructionSystem*>(scene.find_system("Destruction"));
    require(destruction != nullptr, "slice_object: DestructionSystem is installed");

    const std::size_t before = chunks_of(scene).size();
    // The crate is a 2x2x2 cube at (-8,0,1): a plane 100 units away misses it entirely.
    const int result = destruction->slice_object(scene, *crate_obj, glm::vec3(-8.0f, 0.0f, 101.0f),
                                                 glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
    expect(result == 0, "slice_object: a plane entirely outside the mesh misses");
    expect(crate_obj->active(), "slice_object: the object is left alone");
    expect(chunks_of(scene).size() == before, "slice_object: no pieces were spawned");
}
