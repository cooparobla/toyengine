/**
 * @file ik_rig_test.cpp
 * @brief IK for whole rigs: the FABRIK chain solver and ChainIK, the solve order (a spine before
 *        the arms hanging from it, and `priority` overriding it), match_target_rotation, and the
 *        fully IK mannequin -- objects/characters/mannequin_ik playing its baked target clips --
 *        posing every bone where the keyframed mannequin's clips put it.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/animation/animation_clip_loader.h>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/animator.h>
#include <coopa/animation/ik.h>
#include <coopa/animation/ik_components.h>
#include <coopa/animation/ik_system.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/systems/transform_system.h>

#include <root_directory.h>

COOPA_TEST_SUITE("ik_rig");

using namespace coopa::test;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

SceneObject* bone(SceneObject* parent, const std::string& name, const glm::vec3& pos) {
    auto obj = std::make_unique<SceneObject>(name);
    auto* tc = obj->add_component<TransformComponent>();
    tc->transform().set_position(pos);
    tc->set_parent_transform(&parent->get_transform()->transform());
    return parent->add_child(std::move(obj));
}

SceneObject* root_object(Scene& scene, const std::string& name, const glm::vec3& pos) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(pos);
    return scene.add_root_object(std::move(obj));
}

glm::vec3 world(SceneObject* o) { return glm::vec3(o->get_transform()->get_world_matrix()[3]); }

void step(Scene& scene, int frames) {
    for (int i = 0; i < frames; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
}

/** A spine (spine > chest > neck) with a right arm (upper > lower > hand) on the chest. */
struct Torso {
    Scene scene{"torso"};
    SceneObject* root = nullptr;
    SceneObject *spine = nullptr, *chest = nullptr, *neck = nullptr;
    SceneObject *upper = nullptr, *lower = nullptr, *hand = nullptr;
    SceneObject *spine_target = nullptr, *hand_target = nullptr;
    coopa::anim::ChainIK* chain = nullptr;
    coopa::anim::TwoBoneIK* arm = nullptr;

    Torso() {
        root = root_object(scene, "torso", glm::vec3(0.0f));
        spine = bone(root, "spine", glm::vec3(0.0f, 0.0f, 1.0f));
        chest = bone(spine, "chest", glm::vec3(0.0f, 0.0f, 0.3f));
        neck = bone(chest, "neck", glm::vec3(0.0f, 0.0f, 0.3f));
        upper = bone(chest, "upper", glm::vec3(0.2f, 0.0f, 0.2f));
        lower = bone(upper, "lower", glm::vec3(0.0f, 0.0f, -0.3f));
        lower->get_transform()->transform().set_rotation(glm::vec3(20.0f, 0.0f, 0.0f));   // a bend to keep
        hand = bone(lower, "hand", glm::vec3(0.0f, 0.0f, -0.3f));
        spine_target = root_object(scene, "spine_target", glm::vec3(0.25f, 0.1f, 1.5f));
        hand_target = root_object(scene, "hand_target", glm::vec3(0.45f, 0.25f, 1.25f));
        chain = root->add_component<coopa::anim::ChainIK>();
        chain->root = "spine";
        chain->tip = "spine/chest/neck";
        chain->target = "spine_target";
        arm = root->add_component<coopa::anim::TwoBoneIK>();
        arm->upper = "spine/chest/upper";
        arm->lower = "spine/chest/upper/lower";
        arm->end = "spine/chest/upper/lower/hand";
        arm->target = "hand_target";
        arm->soft_limit = 0.0f;
        scene.start();
        coopa::anim::install_ik_system(scene);
    }
};

/** A prefab's rig nodes without its renderers or Animator (the test adds its own Animator). */
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

fkyaml::node read_yaml(const std::string& path) {
    std::ifstream in(path);
    return fkyaml::node::deserialize(in);
}

} // namespace

/** @brief FABRIK: a reachable target is met with every segment's length kept; an unreachable
 *         one straightens the chain toward it. */
COOPA_TEST(chain_solver_reaches_and_keeps_segment_lengths) {
    std::vector<glm::vec3> j = {{0, 0, 0}, {0, 0, 1}, {0, 0.2f, 2}, {0, 0, 3}};
    std::vector<float> len;
    for (size_t i = 0; i + 1 < j.size(); ++i) len.push_back(glm::length(j[i + 1] - j[i]));
    const glm::vec3 target(1.2f, 0.4f, 2.2f);
    expect(coopa::anim::ik::solve_chain(j, target, 20, 1e-4f), "chain: a reachable target is reached");
    expect_near(glm::length(j.back() - target), 0.0f, 1e-3f, "chain: the end sits on the target");
    expect_near(glm::length(j[0]), 0.0f, 1e-6f, "chain: the root stays put");
    for (size_t i = 0; i + 1 < j.size(); ++i)
        expect_near(glm::length(j[i + 1] - j[i]), len[i], 1e-4f, "chain: segment " + std::to_string(i) + " keeps its length");

    std::vector<glm::vec3> far = {{0, 0, 0}, {0, 0, 1}, {0, 0, 2}};
    expect(!coopa::anim::ik::solve_chain(far, glm::vec3(10, 0, 0)), "chain: an unreachable target is not reached");
    expect_near(far.back().x, 2.0f, 1e-4f, "chain: ...the chain points straight at it");
}

/** @brief ChainIK on a spine: the tip reaches its target with the bones' lengths kept, and
 *         weight 0 puts the chain back to its unsolved pose exactly. */
COOPA_TEST(chain_ik_bends_a_spine_onto_its_target) {
    Torso t;
    const glm::vec3 neck_rest = world(t.neck);
    t.arm->weight = 0.0f;
    step(t.scene, 1);
    expect(glm::length(world(t.neck) - world(t.spine_target)) < 2e-3f, "chain ik: the neck reaches the spine target");
    expect_near(glm::length(world(t.chest) - world(t.spine)), 0.3f, 1e-4f, "chain ik: spine > chest keeps its length");
    expect_near(glm::length(world(t.neck) - world(t.chest)), 0.3f, 1e-4f, "chain ik: chest > neck keeps its length");
    expect(t.chain->reached(), "chain ik: reports the target reached");

    t.chain->weight = 0.0f;
    step(t.scene, 1);
    expect(glm::length(world(t.neck) - neck_rest) < 1e-5f, "chain ik: weight 0 restores the unsolved pose");
}

/** @brief Solve order: at equal priority the spine chain solves before the arm on its chest, so
 *         the hand ends on its target; an arm given a lower priority solves first, and the spine
 *         then carries the hand off it. */
COOPA_TEST(spine_solves_before_the_arms_hanging_from_it) {
    {
        Torso t;
        step(t.scene, 1);
        auto* ik = dynamic_cast<coopa::anim::IkSystem*>(t.scene.find_system("IK"));
        expect(ik && ik->chain_count() == 1 && ik->two_bone_count() == 1, "order: the system gathered the chain and the arm");
        if (ik) {
            const auto& order = ik->solve_order();
            expect(order.size() == 2 && order[0] == t.chain && order[1] == t.arm, "order: chain first, then the arm");
        }
        expect(glm::length(world(t.hand) - world(t.hand_target)) < 2e-3f,
               "order: the hand is on its target after the spine moved (" + std::to_string(glm::length(world(t.hand) - world(t.hand_target))) + ")");
    }
    {
        Torso t;
        t.arm->priority = -1;   // the arm first: the spine then moves the chest under it
        step(t.scene, 1);
        expect(glm::length(world(t.hand) - world(t.hand_target)) > 0.02f, "order: priority -1 solves the arm first (the spine then carries the hand off)");
    }
}

/** @brief match_target_rotation: a TwoBoneIK's end bone takes the target's world rotation, and a
 *         ChainIK's tip its own target's. */
COOPA_TEST(match_target_rotation_turns_the_end_bones) {
    Torso t;
    const glm::quat want_hand = glm::quat(glm::radians(glm::vec3(30.0f, -20.0f, 45.0f)));
    const glm::quat want_neck = glm::quat(glm::radians(glm::vec3(-10.0f, 15.0f, 0.0f)));
    t.hand_target->get_transform()->transform().set_rotation_quat(want_hand);
    t.spine_target->get_transform()->transform().set_rotation_quat(want_neck);
    t.arm->match_target_rotation = true;
    t.chain->match_target_rotation = true;
    for (int i = 0; i < 3; ++i) step(t.scene, 1);   // repeated: the guards restore before each solve
    const glm::quat hand = coopa::anim::ik::rotation_of(t.hand->get_transform()->get_world_matrix());
    const glm::quat neck = coopa::anim::ik::rotation_of(t.neck->get_transform()->get_world_matrix());
    expect(std::abs(glm::dot(hand, want_hand)) > 0.9999f, "match rotation: the hand takes its target's rotation");
    expect(std::abs(glm::dot(neck, want_neck)) > 0.9999f, "match rotation: the chain's tip takes its target's rotation");
    expect(glm::length(world(t.hand) - world(t.hand_target)) < 2e-3f, "match rotation: ...and the hand still reaches it");
}

/**
 * @brief The fully IK mannequin moves like the keyframed one: mannequin_ik playing each baked
 *        clip (only target empties keyed) and mannequin playing the keyframed clip put every
 *        joint in the same place, frame for frame.
 */
COOPA_TEST(ik_mannequin_reproduces_the_keyframed_clips) {
    static coopa::asset::AssetManager assets;   // parsers capture it; must outlive them
    coopa::anim::register_animation_components(assets);
    const std::string root_dir = ROOT_DIR;
    const fkyaml::node fk_prefab = read_yaml(root_dir + "/assets/objects/characters/mannequin.yaml");
    const fkyaml::node ik_prefab = read_yaml(root_dir + "/assets/objects/characters/mannequin_ik.yaml");

    const char* joints[] = {"pelvis", "chest", "neck", "head", "hand_l", "hand_r", "lower_arm_l", "lower_arm_r",
                            "shin_l", "shin_r", "foot_l", "foot_r"};
    struct Error { float worst = 0.0f, mean = 0.0f; std::string at; };
    // Plays `clip` on both rigs at a fixed dt for 50 frames; returns the joints' position error.
    auto compare = [&](const char* clip, float dt) {
        Error e;
        fkyaml::node doc = fkyaml::node::mapping();
        doc["scene"] = fkyaml::node::mapping();
        doc["scene"]["scene_name"] = fkyaml::node(std::string("ik_mimic"));
        doc["scene"]["root_objects"] = fkyaml::node::sequence();
        doc["scene"]["root_objects"].get_value_ref<fkyaml::node::sequence_type&>().push_back(strip_rig(fk_prefab["object"]));
        doc["scene"]["root_objects"].get_value_ref<fkyaml::node::sequence_type&>().push_back(strip_rig(ik_prefab["object"]));
        Scene scene = coopa::scene::SceneLoader::load_from_node(doc, (coopa::test::scratch_dir() / "ik_mimic.yaml").string());
        SceneObject* fk_root = scene.find_object("mannequin");
        SceneObject* ik_root = scene.find_object("mannequin_ik");
        if (!fk_root || !ik_root) { e.worst = 1e9f; e.at = "missing rig"; return e; }
        const glm::vec3 apart(3.0f, 0.0f, 0.0f);
        ik_root->get_transform()->transform().set_position(apart);
        auto play = [&](SceneObject* root, const std::string& dir) {
            auto* a = root->add_component<coopa::anim::Animator>();
            a->add_state(clip, std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(
                                   read_yaml(root_dir + "/assets/animations/" + dir + "/" + clip + ".yaml"))));
            a->auto_play = clip;
        };
        play(fk_root, "mannequin");
        play(ik_root, "mannequin_ik");
        scene.start();
        coopa::anim::install_animation_system(scene);
        coopa::anim::install_ik_system(scene);
        coopa::scene::install_transform_system(scene);
        double sum = 0.0;
        int n = 0;
        for (int frame = 0; frame < 50; ++frame) {
            scene.update(dt);
            scene.late_update(dt);
            for (const char* j : joints) {
                SceneObject* a = fk_root->find_descendant(j);
                SceneObject* b = ik_root->find_descendant(j);
                if (!a || !b) { e.worst = 1e9f; e.at = j; continue; }
                const float err = glm::length((world(b) - apart) - world(a));
                sum += err;
                ++n;
                if (err > e.worst) { e.worst = err; e.at = std::string(j) + " @" + std::to_string(frame); }
            }
        }
        e.mean = n ? static_cast<float>(sum / n) : 0.0f;
        return e;
    };
    for (const char* clip : {"idle", "walk", "run", "jump"}) {
        // On the keys (30 fps, the baked clips' frame grid): the same pose, to the millimetre.
        const Error on = compare(clip, 1.0f / 30.0f);
        expect(on.worst < 2e-3f, std::string(clip) + ": on every key the IK mannequin's joints match the keyframed ones (worst " +
                                      std::to_string(on.worst) + " m at " + on.at + ")");
        // Between keys (60 fps): targets interpolate along straight chords while the keyframed
        // limbs swing along arcs, so a near-straight leg's knee can stray for a frame -- a few
        // centimetres at most in the run, and millimetres on average.
        const Error mid = compare(clip, 1.0f / 60.0f);
        expect(mid.mean < 5e-3f && mid.worst < 0.08f,
               std::string(clip) + ": between keys the joints stay close (mean " + std::to_string(mid.mean) + " m, worst " +
                   std::to_string(mid.worst) + " m at " + mid.at + ")");
    }
}
