#pragma once

/**
 * @file horse_rig.h
 * @brief A headless riding scene: the generated objects/animals/horse prefab (its real
 *        CharacterController, Mount and four-legged FootIK, parsed by the real parsers) and a
 *        mannequin_ik player with a CharacterController, FootIK and Rider, on a ground slab, with
 *        every system the engine installs around them. Shared by the mount and foot_ik suites.
 */

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/animator.h>
#include <coopa/animation/ik_system.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/systems/transform_system.h>
#include <coopa/testing/test.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/physics_system.h>
#include <root_directory.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/mount.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/rider.h>

namespace toy::test {

/** @brief `obj` (an object-asset node) without its render parts and Animator. */
inline fkyaml::node strip_render_parts(const fkyaml::node& obj) {
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
            if (!render_part) kids.get_value_ref<fkyaml::node::sequence_type&>().push_back(strip_render_parts(child));
        }
    }
    out["children"] = kids;
    return out;
}

inline fkyaml::node read_yaml_file(const std::string& path) {
    std::ifstream in(path);
    return fkyaml::node::deserialize(in);
}

struct HorseRig {
    coopa::scene::Scene scene{"horse_test"};
    coopa::scene::SceneObject* horse = nullptr;
    coopa::scene::SceneObject* player = nullptr;
    toy::scene::Mount* mount = nullptr;
    toy::scene::CharacterController* horse_cc = nullptr;
    toy::scene::FootIK* hooves = nullptr;
    toy::scene::Rider* rider = nullptr;
    toy::scene::CharacterController* player_cc = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;

    /** @brief The horse at `horse_at` facing `yaw_deg`, and (with_player) the player at `player_at`. */
    HorseRig(const glm::vec3& horse_at, float yaw_deg, bool with_player, const glm::vec3& player_at = glm::vec3(0.0f)) {
        static coopa::asset::AssetManager assets;   // parsers capture it; must outlive them
        coopa::physx::register_physics_components(assets);
        coopa::anim::register_animation_components(assets);
        toy::scene::register_scene_components();
        const std::string root_dir = ROOT_DIR;

        auto place = [](fkyaml::node& n, const glm::vec3& p, float yaw) {
            n["components"][0]["position"]["x"] = fkyaml::node(p.x);
            n["components"][0]["position"]["y"] = fkyaml::node(p.y);
            n["components"][0]["position"]["z"] = fkyaml::node(p.z);
            n["components"][0]["rotation"] = fkyaml::node::mapping();
            n["components"][0]["rotation"]["x"] = fkyaml::node(0.0f);
            n["components"][0]["rotation"]["y"] = fkyaml::node(0.0f);
            n["components"][0]["rotation"]["z"] = fkyaml::node(yaw);
        };
        fkyaml::node doc = fkyaml::node::mapping();
        doc["scene"] = fkyaml::node::mapping();
        doc["scene"]["scene_name"] = fkyaml::node(std::string("horse_test"));
        doc["scene"]["root_objects"] = fkyaml::node::sequence();
        auto& roots = doc["scene"]["root_objects"].get_value_ref<fkyaml::node::sequence_type&>();
        roots.push_back(fkyaml::node::deserialize(std::string(
            "name: ground\n"
            "components:\n"
            "  - {type: Transform, position: {x: 0, y: 0, z: -0.5}}\n"
            "  - {type: BoxCollider, size: {x: 120, y: 120, z: 1}}\n")));
        fkyaml::node h = strip_render_parts(read_yaml_file(root_dir + "/assets/objects/animals/horse.yaml")["object"]);
        place(h, horse_at, yaw_deg);
        roots.push_back(h);
        if (with_player) {
            fkyaml::node p = strip_render_parts(read_yaml_file(root_dir + "/assets/objects/characters/mannequin_ik.yaml")["object"]);
            p["name"] = fkyaml::node(std::string("player"));
            place(p, player_at, 0.0f);
            auto& comps = p["components"].get_value_ref<fkyaml::node::sequence_type&>();
            comps.push_back(fkyaml::node::deserialize(std::string("{type: CharacterController}")));
            comps.push_back(fkyaml::node::deserialize(std::string("{type: FootIK, ray_up: 0.45, ray_down: 0.4}")));
            comps.push_back(fkyaml::node::deserialize(std::string("{type: Rider}")));
            roots.push_back(p);
        }
        scene = coopa::scene::SceneLoader::load_from_node(doc, (coopa::test::scratch_dir() / "horse_test.yaml").string());

        horse = scene.find_object("horse");
        if (horse) {
            mount = horse->get_component<toy::scene::Mount>();
            horse_cc = horse->get_component<toy::scene::CharacterController>();
            hooves = horse->get_component<toy::scene::FootIK>();
            add_animator(horse, "horse", {"idle", "walk", "trot", "gallop"});
        }
        player = scene.find_object("player");
        if (player) {
            rider = player->get_component<toy::scene::Rider>();
            player_cc = player->get_component<toy::scene::CharacterController>();
            add_animator(player, "mannequin_ik", {"idle", "ride"});
        }
    }

    static void add_animator(coopa::scene::SceneObject* obj, const std::string& dir, const std::vector<std::string>& clips) {
        const std::string root_dir = ROOT_DIR;
        auto* a = obj->add_component<coopa::anim::Animator>();
        for (const std::string& c : clips) {
            a->add_state(c, std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(
                                read_yaml_file(root_dir + "/assets/animations/" + dir + "/" + c + ".yaml"))));
        }
        a->auto_play = clips.front();
    }

    /** @brief A static box (before start()). */
    coopa::scene::SceneObject* box(const std::string& name, const glm::vec3& pos, const glm::vec3& size,
                                   const glm::vec3& rot_deg = glm::vec3(0.0f)) {
        auto obj = std::make_unique<coopa::scene::SceneObject>(name);
        auto& t = obj->add_component<coopa::scene::TransformComponent>()->transform();
        t.set_position(pos);
        t.set_rotation(rot_deg);
        obj->add_component<coopa::physx::components::BoxCollider>()->set_size(size);
        return scene.add_root_object(std::move(obj));
    }

    void start() {
        scene.start();
        toy::scene::install_kinematic_control_system(scene);
        physics = coopa::physx::system::install_physics_system(scene);
        coopa::anim::install_animation_system(scene);
        coopa::anim::install_ik_system(scene);
        coopa::scene::install_transform_system(scene);
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            scene.update(1.0f / 60.0f);
            scene.late_update(1.0f / 60.0f);
        }
    }

    /** @brief The engine's input routing for one frame (Engine::drive_character_controllers_()). */
    void input(const glm::vec2& move, bool sprint = false, bool walk = false) {
        if (player_cc && player_cc->player_input && !player_cc->is_suspended()) player_cc->move_input = move;
        if (mount) {
            const bool ridden = mount->is_ridden();
            mount->move_input = ridden ? move : glm::vec2(0.0f);
            mount->move_basis_yaw_deg = 0.0f;
            mount->sprint = ridden && sprint;
            mount->walk = ridden && walk;
        }
    }

    static glm::vec3 world(coopa::scene::SceneObject* o) { return glm::vec3(o->get_transform()->get_world_matrix()[3]); }
    glm::vec3 socket(const std::string& path) const { return world(mount->socket(path)); }
    coopa::scene::SceneObject* bone(coopa::scene::SceneObject* root, const std::string& name) const {
        return root->find_descendant(name);
    }

    /** @brief Teleports the player next to the horse's left mount point, presses interact, and
     *         steps until the climb is over. */
    void mount_up() {
        step(2);
        const glm::vec3 mp = socket(mount->mount_point_l);
        player_cc->teleport(glm::vec3(mp.x - 0.2f, mp.y, mp.z + 0.05f), 0.0f);
        step(10);
        rider->interact = true;
        step(70);
    }
};

} // namespace toy::test
