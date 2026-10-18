/**
 * @file destruction_test.cpp
 * @brief Destructible + DestructionSystem end to end: a dropped weight breaks a mesh into
 *        rigid chunks, the chunks fall and settle, and debris with a lifetime cleans itself up.
 *
 * The fracture maths has its own device-free suite (tests/engine/unit/fracture_test.cpp). What
 * is checked here is everything that only exists at runtime: the mesh being read and cut on a
 * worker, the impulse threshold firing on a real collision, the chunk objects carrying a body
 * and a collider, the original going away, and the cleanup pass removing the debris again.
 */

#include <coopa/testing/test.h>

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/destructible.h>
#include <toyengine/scene/destruction_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("destruction");

using namespace toy::test;

namespace {

/** @brief Every chunk object the destruction system has spawned into `scene`. */
std::vector<coopa::scene::SceneObject*> chunks_of(coopa::scene::Scene& scene) {
    std::vector<coopa::scene::SceneObject*> out;
    for (const auto& object : scene.root_objects()) {
        if (object->name().rfind("chunk_", 0) == 0) out.push_back(object.get());
    }
    return out;
}

toy::scene::Destructible* breakable(coopa::scene::Scene& scene, const std::string& name) {
    auto* object = scene.find_object(name);
    return object ? object->get_component<toy::scene::Destructible>() : nullptr;
}

} // namespace

/**
 * @brief The demo's own setup: a weight falls on a crate, the crate becomes chunks, and the
 *        chunks are bodies that come to rest on the ground rather than falling through it.
 */
COOPA_TEST(a_dropped_weight_breaks_a_mesh_into_physical_chunks) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/destruction_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* crate = breakable(scene, "crate");
    require(crate != nullptr, "destruction: the demo scene carries a Destructible crate");
    expect(!crate->shattered(), "destruction: it starts whole");

    // The weight above it needs about a second to arrive.
    const int frames = tick_until(engine, 300, [&] { return crate->shattered(); });
    expect(crate->shattered(), "destruction: the impact broke it (after " + std::to_string(frames) + " frames)");
    expect(crate->chunk_count() > 4, "destruction: into several chunks (" +
                                         std::to_string(crate->chunk_count()) + ")");
    expect(crate->owner != nullptr && !crate->owner->active(),
           "destruction: ...and the original object is gone from the scene's walks");

    std::vector<coopa::scene::SceneObject*> chunks = chunks_of(scene);
    require(!chunks.empty(), "destruction: the chunks are in the scene");
    int with_body = 0, with_collider = 0, compound = 0;
    float heaviest = 0.0f;
    for (coopa::scene::SceneObject* chunk : chunks) {
        auto* body = chunk->get_component<coopa::physx::components::RigidbodyComponent>();
        if (body != nullptr) { ++with_body; heaviest = std::max(heaviest, body->mass); }
        // A shard that one box fits badly is given several, as child colliders -- PhysicsSystem
        // gathers those into one compound body (destruction_system.h), so the collider can be
        // on the chunk itself OR on its children.
        int boxes = chunk->get_component<coopa::physx::components::BoxCollider>() != nullptr ? 1 : 0;
        for (const auto& child : chunk->children()) {
            boxes += child->get_component<coopa::physx::components::BoxCollider>() != nullptr ? 1 : 0;
        }
        if (boxes > 0) ++with_collider;
        if (boxes > 1) ++compound;
    }
    expect(with_body == static_cast<int>(chunks.size()), "destruction: every chunk is a rigid body");
    expect(with_collider == static_cast<int>(chunks.size()), "destruction: ...with a collider");
    expect(compound > 0, "destruction: ...and awkward shards collide as several boxes, not one (" +
                             std::to_string(compound) + " of " + std::to_string(chunks.size()) + ")");
    expect(heaviest > 0.0f, "destruction: ...weighed from its own volume (heaviest " +
                                std::to_string(heaviest) + " kg)");

    // They fall, and the ground stops them: nothing below it two seconds later.
    tick_frames(engine, 150);
    int below_ground = 0;
    float lowest = 1e9f;
    for (coopa::scene::SceneObject* chunk : chunks_of(scene)) {
        const float z = chunk->get_transform()->transform().position().z;
        lowest = std::min(lowest, z);
        if (z < -2.0f) ++below_ground;
    }
    expect(below_ground == 0, "destruction: the chunks rest on the ground rather than falling through (lowest z " +
                                  std::to_string(lowest) + ")");
}

/** @brief Debris with a lifetime is cleaned up; debris without one stays. */
COOPA_TEST(chunks_with_a_lifetime_are_cleaned_up) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config("assets/scenes/physics/destruction_demo/scene.yaml", 320, 180, 160, 90));
    coopa::scene::Scene& scene = engine.scene();
    auto* mushroom = breakable(scene, "mushroom");
    require(mushroom != nullptr, "cleanup: the demo scene carries a Destructible mushroom");

    // Short-lived debris, broken by hand rather than waiting for its weight.
    mushroom->chunk_lifetime = 0.4f;
    mushroom->chunk_fade = 0.1f;
    tick_frames(engine, 20);                       // let the prefracture land
    mushroom->shatter(glm::vec3(8.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    tick_until(engine, 120, [&] { return mushroom->shattered(); });
    require(mushroom->shattered(), "cleanup: shatter() breaks it without an impact");

    // This object's chunks by name, not every chunk in the scene: the demo's other weights are
    // still falling and breaking things while this runs, and counting those would hide the
    // cleanup entirely (it did: "67 left of 8").
    std::vector<std::string> mine;
    for (coopa::scene::SceneObject* chunk : chunks_of(scene)) mine.push_back(chunk->name());
    expect(!mine.empty(), "cleanup: it produced chunks (" + std::to_string(mine.size()) + ")");

    tick_frames(engine, 120);   // ~2 s: well past lifetime + fade
    std::size_t left = 0;
    for (const std::string& name : mine) left += scene.find_object(name) != nullptr ? 1 : 0;
    expect(left == 0, "cleanup: the short-lived debris was removed (" + std::to_string(left) +
                          " left of " + std::to_string(mine.size()) + ")");
}
