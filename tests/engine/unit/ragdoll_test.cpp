/**
 * @file ragdoll_test.cpp
 * @brief Ragdoll on the generated mannequin_ragdoll prefab (its real config, real parser): settling
 *        without exploding, joints holding under a shove, a sleeping body pushed by a kinematic ram
 *        (a past explosion), and the Animated -> Ragdoll -> Animated round trip.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/ik_system.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/systems/transform_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/ragdoll.h>
#include <toyengine/scene/register.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("ragdoll");

using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace ragdoll_test {

/** @brief `obj` (an object-asset node) without its render parts and Animator: only Transforms,
 *         the Ragdoll and whatever else headless parsers know. */
fkyaml::node strip_rig(const fkyaml::node& obj) {
    fkyaml::node out = fkyaml::node::mapping();
    out["name"] = obj["name"];
    fkyaml::node comps = fkyaml::node::sequence();
    for (const auto& c : obj["components"]) {
        const std::string type = c["type"].get_value<std::string>();
        if (type == "Animator" || type == "MeshRenderer") continue;
        comps.get_value_ref<fkyaml::node::sequence_type&>().push_back(c);
    }
    out["components"] = comps;
    fkyaml::node kids = fkyaml::node::sequence();
    if (obj.contains("children") && obj["children"].is_sequence()) {
        for (const auto& child : obj["children"]) {
            bool render_part = false;
            for (const auto& c : child["components"]) render_part = render_part || c["type"].get_value<std::string>() == "MeshRenderer";
            if (!render_part) kids.get_value_ref<fkyaml::node::sequence_type&>().push_back(strip_rig(child));
        }
    }
    out["children"] = kids;
    return out;
}

/**
 * @brief The generated mannequin_ragdoll prefab (its real Ragdoll config, parsed by the real
 *        "Ragdoll" parser) on a ground slab, with the idle clip on an Animator, an optional
 *        CharacterController, and every system the engine installs around a ragdoll.
 */
struct Rig {
    coopa::scene::Scene scene{"ragdoll_test"};
    SceneObject* root = nullptr;
    toy::scene::Ragdoll* ragdoll = nullptr;
    coopa::anim::Animator* animator = nullptr;
    toy::scene::CharacterController* cc = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;

    explicit Rig(bool with_character, const glm::vec3& at = glm::vec3(0.0f)) {
        static coopa::asset::AssetManager assets;   // parsers capture it; must outlive them
        coopa::physx::register_physics_components(assets);
        toy::scene::register_scene_components();
        const std::string root_dir = ROOT_DIR;
        std::ifstream in(root_dir + "/assets/objects/characters/mannequin_ragdoll.yaml");
        fkyaml::node prefab = fkyaml::node::deserialize(in);
        fkyaml::node rig = strip_rig(prefab["object"]);
        rig["components"][0]["position"]["x"] = fkyaml::node(at.x);
        rig["components"][0]["position"]["y"] = fkyaml::node(at.y);
        rig["components"][0]["position"]["z"] = fkyaml::node(at.z);
        fkyaml::node ground = fkyaml::node::deserialize(std::string(
            "name: ground\n"
            "components:\n"
            "  - {type: Transform, position: {x: 0, y: 0, z: -0.5}}\n"
            "  - {type: BoxCollider, size: {x: 40, y: 40, z: 1}}\n"));
        fkyaml::node doc = fkyaml::node::mapping();
        doc["scene"] = fkyaml::node::mapping();
        doc["scene"]["scene_name"] = fkyaml::node(std::string("ragdoll_test"));
        doc["scene"]["root_objects"] = fkyaml::node::sequence();
        doc["scene"]["root_objects"].get_value_ref<fkyaml::node::sequence_type&>().push_back(ground);
        doc["scene"]["root_objects"].get_value_ref<fkyaml::node::sequence_type&>().push_back(rig);
        scene = coopa::scene::SceneLoader::load_from_node(doc, (coopa::test::scratch_dir() / "ragdoll_test.yaml").string());

        root = scene.find_object("mannequin_ragdoll");
        if (!root) return;
        ragdoll = root->get_component<toy::scene::Ragdoll>();
        std::ifstream clip_in(root_dir + "/assets/animations/mannequin/idle.yaml");
        animator = root->add_component<coopa::anim::Animator>();
        animator->add_state("idle", std::make_shared<coopa::anim::AnimationClip>(
                                        coopa::anim::parse_clip(fkyaml::node::deserialize(clip_in))));
        animator->auto_play = "idle";
        if (with_character) cc = root->add_component<toy::scene::CharacterController>();

        scene.start();
        toy::scene::install_kinematic_control_system(scene);
        physics = coopa::physx::system::install_physics_system(scene);
        coopa::anim::install_animation_system(scene);
        coopa::anim::install_ik_system(scene);
        toy::scene::install_ragdoll_system(scene);
        coopa::scene::install_transform_system(scene);
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            scene.update(1.0f / 60.0f);
            scene.late_update(1.0f / 60.0f);
        }
    }

    glm::vec3 world(size_t bone) const {
        return glm::vec3(ragdoll->bone_object(bone)->get_transform()->transform().get_world_matrix()[3]);
    }

    const coopa::physx::dynamics::Body* body(size_t bone) const {
        return physics->world().get_body(ragdoll->bone_body(bone));
    }

    /** @brief Total kinetic energy (J) of the bone bodies. */
    float kinetic_energy() const {
        float e = 0.0f;
        for (size_t i = 0; i < ragdoll->bone_count(); ++i) {
            const auto* b = body(i);
            if (!b || b->type != coopa::physx::dynamics::BodyType::Dynamic) continue;
            e += 0.5f * b->mass * glm::dot(b->linear_velocity, b->linear_velocity);
            const glm::vec3 L = glm::inverse(b->inv_inertia_world) * b->angular_velocity;
            e += 0.5f * glm::dot(b->angular_velocity, L);
        }
        return e;
    }

    /** @brief Largest gap between a joint's two anchors (m) -- 0 when every joint holds. */
    float max_joint_gap() const {
        float gap = 0.0f;
        for (size_t i = 0; i < ragdoll->bone_count(); ++i) {
            const coopa::physx::dynamics::Joint* j = physics->world().get_joint(ragdoll->bone_joint(i));
            if (!j) continue;
            const auto* a = physics->world().get_body(j->a);
            const auto* b = physics->world().get_body(j->b);
            gap = std::max(gap, glm::length((a->position + a->orientation * j->local_anchor_a) -
                                            (b->position + b->orientation * j->local_anchor_b)));
        }
        return gap;
    }

    int find_bone(const std::string& name) const {
        for (size_t i = 0; i < ragdoll->bone_count(); ++i)
            if (ragdoll->bone_object(i)->name() == name) return static_cast<int>(i);
        return -1;
    }
};

} // namespace ragdoll_test

/**
 * @brief A mannequin dropped limp from standing height settles on the ground without exploding:
 *        every body stays finite and slower than a real fall could make it, and after 300
 *        frames (5 s) the total kinetic energy is tiny and the body lies near where it fell.
 */
COOPA_TEST(settles_without_exploding) {
    ragdoll_test::Rig rig(false, glm::vec3(0.0f, 0.0f, 0.5f));   // feet half a metre up
    expect(rig.ragdoll != nullptr && rig.ragdoll->bone_count() == 14, "ragdoll: the mannequin prefab's 14 bones resolve");
    if (!rig.ragdoll || rig.ragdoll->bone_count() == 0) return;
    rig.step(2);
    expect(rig.ragdoll->ready(), "ragdoll: joints exist once the bodies are bound");
    rig.ragdoll->activate();
    float max_speed = 0.0f;
    bool finite = true;
    for (int f = 0; f < 300; ++f) {
        rig.step(1);
        for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) {
            const auto* b = rig.body(i);
            if (!b) continue;
            max_speed = std::max(max_speed, glm::length(b->linear_velocity));
            finite = finite && std::isfinite(b->position.x) && std::isfinite(b->position.z) && std::isfinite(b->angular_velocity.x);
        }
    }
    expect(rig.ragdoll->is_ragdoll(), "ragdoll: activate() went limp");
    expect(finite, "ragdoll: every body stays finite");
    // A free fall from 1.9 m tops out near 6 m/s; anything far beyond is the solver injecting energy.
    expect(max_speed < 9.0f, "ragdoll: no body is flung (max speed " + std::to_string(max_speed) + " m/s)");
    const float ke = rig.kinetic_energy();
    expect(ke < 2.0f, "ragdoll: settled after 300 frames (kinetic energy " + std::to_string(ke) + " J)");
    const glm::vec3 pelvis = rig.world(0);
    expect(pelvis.z > 0.0f && pelvis.z < 0.45f, "ragdoll: the pelvis lies on the ground (z " + std::to_string(pelvis.z) + ")");
    expect(glm::length(glm::vec2(pelvis)) < 1.5f, "ragdoll: the body crumpled where it fell");
    for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) {
        expect(rig.world(i).z > -0.05f, "ragdoll: no bone sank through the ground (" + rig.ragdoll->bone_object(i)->name() + ")");
    }
}

/**
 * @brief Jointed bones stay jointed: shoved hard sideways at the chest, the falling body's joint
 *        anchors stay together and every bone's pivot keeps its distance to its parent bone's
 *        pivot (read from the written-back Transforms, which also checks the hierarchical
 *        write-back: a child bone lands at its body's pose relative to the MOVED parent).
 */
COOPA_TEST(joints_hold_under_a_hard_shove) {
    ragdoll_test::Rig rig(false);
    if (!rig.ragdoll || rig.ragdoll->bone_count() == 0) { expect(false, "ragdoll distances: rig built"); return; }
    rig.step(2);
    const size_t n = rig.ragdoll->bone_count();
    std::vector<float> rest(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const int p = rig.ragdoll->bone_parent(i);
        if (p >= 0) rest[i] = glm::length(rig.world(i) - rig.world(static_cast<size_t>(p)));
    }
    const int chest = rig.find_bone("chest");
    rig.ragdoll->activate(glm::vec3(60.0f, 20.0f, 0.0f), rig.world(static_cast<size_t>(std::max(chest, 0))));
    float max_gap = 0.0f, max_stretch = 0.0f, max_body_vs_transform = 0.0f;
    for (int f = 0; f < 180; ++f) {
        rig.step(1);
        max_gap = std::max(max_gap, rig.max_joint_gap());
        for (size_t i = 0; i < n; ++i) {
            const int p = rig.ragdoll->bone_parent(i);
            if (p >= 0) max_stretch = std::max(max_stretch, std::abs(glm::length(rig.world(i) - rig.world(static_cast<size_t>(p))) - rest[i]));
            const auto* b = rig.body(i);
            // The Transform pivot + the body's own centre offset lands on the written body centre.
            const glm::mat4 m = rig.ragdoll->bone_object(i)->get_transform()->transform().get_world_matrix();
            const glm::vec3 com = rig.ragdoll->bone_object(i)->get_component<coopa::physx::components::RigidbodyComponent>()->local_center_of_mass();
            max_body_vs_transform = std::max(max_body_vs_transform,
                                             glm::length(glm::vec3(m * glm::vec4(com, 1.0f)) - b->last_written_position));
        }
    }
    expect(rig.ragdoll->is_ragdoll(), "ragdoll distances: limp");
    expect(glm::length(glm::vec2(rig.world(0))) > 0.2f, "ragdoll distances: the shove moved the body");
    expect(max_gap < 0.03f, "ragdoll distances: joint anchors stay together (max gap " + std::to_string(max_gap) + " m)");
    expect(max_stretch < 0.03f, "ragdoll distances: bone-to-parent distances hold (max " + std::to_string(max_stretch) + " m)");
    expect(max_body_vs_transform < 0.005f,
           "ragdoll distances: every bone Transform matches its body (max " + std::to_string(max_body_vs_transform) + " m)");
}

/**
 * @brief A settled (asleep) ragdoll shoved by a kinematic ram is pushed along, not blown apart:
 *        the ram wakes the bone it touches and the joints wake the rest of the body with it (a
 *        lone woken bone pinned between the ram and still-sleeping neighbours used to explode).
 */
COOPA_TEST(sleeping_body_is_pushed_by_a_ram_not_blown_apart) {
    ragdoll_test::Rig rig(false);
    if (!rig.ragdoll || rig.ragdoll->bone_count() == 0) { expect(false, "ragdoll ram: rig built"); return; }
    rig.step(2);
    rig.ragdoll->activate();
    rig.step(300);   // collapse and fall asleep (Ragdoll's rest assist)
    bool asleep = true;
    for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) asleep = asleep && !rig.body(i)->awake;
    expect(asleep, "ragdoll ram: the collapsed body fell asleep");
    const glm::vec3 lying = rig.world(0);

    // The ram: a kinematic slab sweeping +X through where the body lies, 3 m/s.
    auto ram_obj = std::make_unique<SceneObject>("ram");
    auto& rt = ram_obj->add_component<TransformComponent>()->transform();
    rt.set_position(glm::vec3(lying.x - 2.0f, lying.y, 0.45f));
    ram_obj->add_component<coopa::physx::components::BoxCollider>()->set_size(glm::vec3(0.6f, 3.0f, 0.9f));
    auto* rb = ram_obj->add_component<coopa::physx::components::RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    SceneObject* ram = rig.scene.add_root_object(std::move(ram_obj));
    rig.physics->refresh();
    float max_speed = 0.0f;
    for (int f = 0; f < 90; ++f) {
        ram->get_transform()->transform().set_position(glm::vec3(lying.x - 2.0f + 3.0f * (f + 1) / 60.0f, lying.y, 0.45f));
        rig.step(1);
        for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) max_speed = std::max(max_speed, glm::length(rig.body(i)->linear_velocity));
    }
    const glm::vec3 after = rig.world(0);
    expect(after.x > lying.x + 0.5f, "ragdoll ram: the body was pushed along +X (" + std::to_string(after.x - lying.x) + " m)");
    expect(max_speed < 8.0f, "ragdoll ram: nothing flung (max speed " + std::to_string(max_speed) + " m/s)");
    expect(rig.max_joint_gap() < 0.03f, "ragdoll ram: the joints held (gap " + std::to_string(rig.max_joint_gap()) + " m)");
    expect(after.z > -0.05f && after.z < 1.0f, "ragdoll ram: still on the ground");
}

/**
 * @brief Animated -> Ragdoll -> Animated with a CharacterController: animated bones are kinematic
 *        and hittable, activate() goes limp (Animator cleared, controller suspended, capsule off),
 *        deactivate() holds the fallen pose without a pop, stands the character up under where
 *        the pelvis lay, blends into idle, and ends kinematic and animated again.
 */
COOPA_TEST(animated_to_ragdoll_and_back_round_trips) {
    using coopa::physx::dynamics::BodyType;
    ragdoll_test::Rig rig(true);
    if (!rig.ragdoll || rig.ragdoll->bone_count() == 0 || !rig.cc) { expect(false, "ragdoll round trip: rig built"); return; }
    rig.step(30);
    auto* cap = rig.root->get_component<coopa::physx::components::CapsuleCollider>();
    bool all_kinematic = true;
    for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) all_kinematic = all_kinematic && rig.body(i)->type == BodyType::Kinematic;
    expect(rig.ragdoll->mode() == toy::scene::Ragdoll::Mode::Animated && all_kinematic, "ragdoll round trip: animated bones are kinematic");
    expect_near(rig.world(0).z, 0.95f, 0.02f, "ragdoll round trip: the animated pelvis stands at hip height");
    {
        // Hits register: a ray at the head finds the head bone's collider (not the capsule's
        // object -- the ray's filter skips the root's own body).
        coopa::physx::system::PhysicsSystem::QueryFilter filter;
        filter.ignore_rigidbody = rig.root->get_component<coopa::physx::components::RigidbodyComponent>();
        coopa::physx::geometry::Ray ray;
        ray.origin = glm::vec3(0.0f, -2.0f, rig.world(static_cast<size_t>(rig.find_bone("head"))).z + 0.11f);
        ray.direction = glm::vec3(0.0f, 1.0f, 0.0f);
        ray.max_distance = 4.0f;
        coopa::physx::system::PhysicsSystem::RaycastHit hit;
        const bool got = rig.physics->raycast(ray, hit, filter);
        expect(got && hit.collider && hit.collider->owner->name() == "head", "ragdoll round trip: a ray hits the animated head");
    }

    rig.ragdoll->activate(glm::vec3(0.0f, 150.0f, 0.0f));
    rig.step(1);
    all_kinematic = true;
    bool all_dynamic = true;
    for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) all_dynamic = all_dynamic && rig.body(i)->type == BodyType::Dynamic;
    expect(rig.ragdoll->is_ragdoll() && all_dynamic, "ragdoll round trip: activate() makes every bone dynamic");
    expect(!rig.animator->is_playing() && rig.animator->current_state().empty(), "ragdoll round trip: the Animator is cleared");
    expect(rig.cc->is_suspended() && cap && !cap->is_enabled(), "ragdoll round trip: the controller is suspended, its capsule off");
    const glm::vec3 root_before = rig.root->get_transform()->transform().position();
    rig.step(150);
    const glm::vec3 lying = rig.world(0);
    expect(lying.z < 0.5f, "ragdoll round trip: the shoved body fell (pelvis z " + std::to_string(lying.z) + ")");
    expect(glm::length(glm::vec2(lying - root_before)) > 0.3f, "ragdoll round trip: ...away from where it stood");

    std::vector<glm::vec3> before(rig.ragdoll->bone_count());
    for (size_t i = 0; i < before.size(); ++i) before[i] = rig.world(i);
    rig.ragdoll->deactivate(0.5f, "idle");
    rig.step(1);
    float pop = 0.0f;
    for (size_t i = 0; i < before.size(); ++i) pop = std::max(pop, glm::length(rig.world(i) - before[i]));
    expect(rig.ragdoll->mode() == toy::scene::Ragdoll::Mode::Blending, "ragdoll round trip: deactivate() starts the blend");
    expect(pop < 0.05f, "ragdoll round trip: the first blend frame holds the fallen pose (moved " + std::to_string(pop) + " m)");
    const glm::vec3 root_after = rig.root->get_transform()->transform().position();
    expect(glm::length(glm::vec2(root_after - lying)) < 0.05f && std::abs(root_after.z) < 0.05f,
           "ragdoll round trip: the character stands up under where the pelvis lay");
    expect(!rig.cc->is_suspended() && cap && cap->is_enabled(), "ragdoll round trip: the controller is back, capsule on");

    rig.step(40);   // past the 0.5 s blend
    all_kinematic = true;
    for (size_t i = 0; i < rig.ragdoll->bone_count(); ++i) all_kinematic = all_kinematic && rig.body(i)->type == BodyType::Kinematic;
    expect(rig.ragdoll->mode() == toy::scene::Ragdoll::Mode::Animated && all_kinematic,
           "ragdoll round trip: the blend ends animated, bones kinematic");
    expect(rig.animator->is_playing() && rig.animator->current_state() == "idle", "ragdoll round trip: the Animator plays idle");
    const glm::vec3 pelvis = rig.world(0);
    expect_near(pelvis.z, root_after.z + 0.95f, 0.03f, "ragdoll round trip: the pelvis is back at hip height");
    expect(glm::length(glm::vec2(pelvis - root_after)) < 0.05f, "ragdoll round trip: ...over the new root");

    // And again: a second activation works from the recovered state.
    rig.ragdoll->activate();
    rig.step(2);
    expect(rig.ragdoll->is_ragdoll(), "ragdoll round trip: it can go limp again");
}
