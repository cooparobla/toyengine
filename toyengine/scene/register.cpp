#include <toyengine/scene/register.h>

#include <filesystem>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/yaml/document.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/memory/allocator.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/character_anim_driver.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/scene/health_driver.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>
#include <toyengine/scene/mount.h>
#include <toyengine/scene/ragdoll.h>
#include <toyengine/scene/rider.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/scene/skinned_mesh_renderer.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>

namespace toy {
namespace scene {

glm::vec3 parse_vec3(const fkyaml::node& n, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (n.contains("x")) v.x = n.at("x").get_value<float>();
    if (n.contains("y")) v.y = n.at("y").get_value<float>();
    if (n.contains("z")) v.z = n.at("z").get_value<float>();
    return v;
}

glm::vec3 parse_rgb(const fkyaml::node& n, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (n.contains("r")) v.r = n.at("r").get_value<float>();
    if (n.contains("g")) v.g = n.at("g").get_value<float>();
    if (n.contains("b")) v.b = n.at("b").get_value<float>();
    return v;
}

void register_scene_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("CameraController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<CameraController>();

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                if (m == "fly" || m == "Fly") cc->mode = CameraControlMode::Fly;
                else if (m == "first_person" || m == "FirstPerson") cc->mode = CameraControlMode::FirstPerson;
                else cc->mode = CameraControlMode::Orbit;
            }

            if (node.contains("tracker")) {
                cc->tracker = node.at("tracker").get_value<std::string>();
            }
            if (node.contains("target")) {
                cc->target = parse_vec3(node.at("target"), cc->target);
            }
            if (node.contains("target_offset")) {
                cc->target_offset = parse_vec3(node.at("target_offset"), cc->target_offset);
            }
            if (node.contains("follow_smoothing")) {
                cc->follow_smoothing = node.at("follow_smoothing").get_value<float>();
            }
            if (node.contains("movement_smoothing")) {
                cc->movement_smoothing = node.at("movement_smoothing").get_value<float>();
            }

            if (node.contains("distance")) cc->distance = node.at("distance").get_value<float>();
            if (node.contains("yaw_deg"))   cc->yaw_deg   = node.at("yaw_deg").get_value<float>();
            if (node.contains("pitch_deg")) cc->pitch_deg = node.at("pitch_deg").get_value<float>();

            if (node.contains("min_pitch_deg")) cc->min_pitch_deg = node.at("min_pitch_deg").get_value<float>();
            if (node.contains("max_pitch_deg")) cc->max_pitch_deg = node.at("max_pitch_deg").get_value<float>();
            if (node.contains("min_distance"))  cc->min_distance  = node.at("min_distance").get_value<float>();
            if (node.contains("max_distance"))  cc->max_distance  = node.at("max_distance").get_value<float>();

            if (node.contains("mouse_sensitivity")) {
                cc->mouse_sensitivity = node.at("mouse_sensitivity").get_value<float>();
            }
            if (node.contains("invert_x")) cc->invert_x = node.at("invert_x").get_value<bool>();
            if (node.contains("invert_y")) cc->invert_y = node.at("invert_y").get_value<bool>();
            if (node.contains("zoom_speed")) cc->zoom_speed = node.at("zoom_speed").get_value<float>();
            if (node.contains("capture_cursor")) {
                cc->capture_cursor = node.at("capture_cursor").get_value<bool>();
            }

            if (node.contains("auto_rotate_deg_per_sec")) {
                cc->auto_rotate_deg_per_sec = node.at("auto_rotate_deg_per_sec").get_value<float>();
            }
            if (node.contains("move_speed")) {
                cc->move_speed = node.at("move_speed").get_value<float>();
            }
            if (node.contains("look_speed_deg_per_sec")) {
                cc->look_speed_deg_per_sec = node.at("look_speed_deg_per_sec").get_value<float>();
            }
            if (node.contains("collide")) cc->collide = node.at("collide").get_value<bool>();
            if (node.contains("collision_radius")) cc->collision_radius = node.at("collision_radius").get_value<float>();
            if (node.contains("collision_in_speed")) cc->collision_in_speed = node.at("collision_in_speed").get_value<float>();
            if (node.contains("collision_out_speed")) cc->collision_out_speed = node.at("collision_out_speed").get_value<float>();
            if (node.contains("eye_height")) cc->eye_height = node.at("eye_height").get_value<float>();
            if (node.contains("first_person_pitch_limit"))
                cc->first_person_pitch_limit = node.at("first_person_pitch_limit").get_value<float>();
        });

    // A walking capsule character (toyengine/scene/character_controller.h). Input is pushed in by
    // Engine::drive_character_controllers_(); it adds its own kinematic Rigidbody + CapsuleCollider.
    SceneLoader::register_component_parser("CharacterController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<CharacterController>();
            auto f = [&](const char* key, float& out) {
                if (node.contains(key)) out = node.at(key).get_value<float>();
            };
            auto b = [&](const char* key, bool& out) {
                if (node.contains(key)) out = node.at(key).get_value<bool>();
            };
            f("radius", cc->radius);
            f("height", cc->height);
            f("step_height", cc->step_height);
            f("slope_limit", cc->slope_limit);
            f("skin", cc->skin);
            f("snap_distance", cc->snap_distance);
            f("move_speed", cc->move_speed);
            f("sprint_multiplier", cc->sprint_multiplier);
            f("acceleration", cc->acceleration);
            f("air_control", cc->air_control);
            f("gravity_scale", cc->gravity_scale);
            f("jump_height", cc->jump_height);
            f("coyote_time", cc->coyote_time);
            f("jump_buffer", cc->jump_buffer);
            b("face_movement", cc->face_movement);
            f("turn_speed", cc->turn_speed);
            b("push_dynamic_bodies", cc->push_dynamic_bodies);
            f("push_strength", cc->push_strength);
            b("use_root_motion", cc->use_root_motion);
            b("player_input", cc->player_input);
        });

    // A rideable animal (toyengine/scene/mount.h): steers its own CharacterController from the
    // rider's input, plays its gait clips, and names the sockets a Rider sits on / reaches for.
    SceneLoader::register_component_parser("Mount",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* m = obj.add_component<Mount>();
            auto f = [&](const char* key, float& out) {
                if (node.contains(key)) out = node.at(key).get_value<float>();
            };
            auto str = [&](const char* key, std::string& out) {
                if (node.contains(key)) out = node.at(key).get_value<std::string>();
            };
            f("walk_speed", m->walk_speed);
            f("trot_speed", m->trot_speed);
            f("gallop_speed", m->gallop_speed);
            f("acceleration", m->acceleration);
            f("braking", m->braking);
            f("turn_rate", m->turn_rate);
            f("gallop_turn_rate", m->gallop_turn_rate);
            f("mount_max_speed", m->mount_max_speed);
            str("idle_state", m->idle_state);
            str("walk_state", m->walk_state);
            str("trot_state", m->trot_state);
            str("gallop_state", m->gallop_state);
            f("crossfade", m->crossfade);
            f("min_anim_speed", m->min_anim_speed);
            f("max_anim_speed", m->max_anim_speed);
            str("saddle", m->saddle);
            str("stirrup_l", m->stirrup_l);
            str("stirrup_r", m->stirrup_r);
            str("rein_l", m->rein_l);
            str("rein_r", m->rein_r);
            str("mount_point_l", m->mount_point_l);
            str("mount_point_r", m->mount_point_r);
            f("camera_offset_z", m->camera_offset_z);
            f("camera_distance", m->camera_distance);
        });

    // Gets the player on / off a Mount (toyengine/scene/rider.h); `interact` (F) is pushed by
    // Engine::drive_character_controllers_().
    SceneLoader::register_component_parser("Rider",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* r = obj.add_component<Rider>();
            auto f = [&](const char* key, float& out) {
                if (node.contains(key)) out = node.at(key).get_value<float>();
            };
            f("mount_radius", r->mount_radius);
            f("mount_time", r->mount_time);
            f("dismount_time", r->dismount_time);
            if (node.contains("ride_state")) r->ride_state = node.at("ride_state").get_value<std::string>();
            f("crossfade", r->crossfade);
            if (node.contains("seat_offset")) r->seat_offset = parse_vec3(node.at("seat_offset"), r->seat_offset);
            f("knee_spread", r->knee_spread);
            f("lean", r->lean);
            f("leg_weight", r->leg_weight);
            f("arm_weight", r->arm_weight);
            if (node.contains("priority")) r->priority = node.at("priority").get_value<int>();
            if (node.contains("mount_on_start")) r->mount_on_start = node.at("mount_on_start").get_value<std::string>();
        });

    // A trigger box that loads another scene when the player walks in (toyengine/scene/scene_link.h).
    // A relative target_scene resolves against the declaring file first (a sibling scene), then
    // the asset roots; anything still unresolved goes to the engine's scene lookup by name.
    SceneLoader::register_component_parser("SceneLink",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* link = obj.add_component<SceneLink>();
            auto str = [&](const char* key, std::string& out) {
                if (node.contains(key)) out = node.at(key).get_value<std::string>();
            };
            str("target_scene", link->target_scene);
            if (!link->target_scene.empty()) link->target_scene = ctx.resolve(link->target_scene);
            str("transition", link->transition);
            str("loading_screen", link->loading_screen);
            str("spawn_point", link->spawn_point);
            if (node.contains("color")) link->color = parse_rgb(node.at("color"), link->color);
            if (node.contains("fade_time")) link->fade_time = node.at("fade_time").get_value<float>();
            if (node.contains("min_display_time")) link->min_display_time = node.at("min_display_time").get_value<float>();
            if (node.contains("size")) link->size = parse_vec3(node.at("size"), link->size);
        });

    // Demo: crossfades the Animator between idle/walk/run/jump from the CharacterController.
    SceneLoader::register_component_parser("CharacterAnimDriver",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<CharacterAnimDriver>();
            if (node.contains("idle_state")) d->idle_state = node.at("idle_state").get_value<std::string>();
            if (node.contains("walk_state")) d->walk_state = node.at("walk_state").get_value<std::string>();
            if (node.contains("run_state")) d->run_state = node.at("run_state").get_value<std::string>();
            if (node.contains("jump_state")) d->jump_state = node.at("jump_state").get_value<std::string>();
            if (node.contains("walk_speed")) d->walk_speed = node.at("walk_speed").get_value<float>();
            if (node.contains("run_speed")) d->run_speed = node.at("run_speed").get_value<float>();
            if (node.contains("crossfade")) d->crossfade = node.at("crossfade").get_value<float>();
            if (node.contains("air_delay")) d->air_delay = node.at("air_delay").get_value<float>();
        });

    // Ground-adapting legs (toyengine/scene/foot_ik.h), solved by coopa::anim::IkSystem.
    SceneLoader::register_component_parser("FootIK",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<FootIK>();
            auto f = [&](const char* key, float& out) {
                if (node.contains(key)) out = node.at(key).get_value<float>();
            };
            // A leg: [thigh, shin, foot] or {thigh, shin, foot, pole, front}.
            auto parse_leg = [&](const fkyaml::node& n, FootIK::Leg& out) {
                if (n.is_sequence()) {
                    if (n.size() > 0) out.thigh = n[0].get_value<std::string>();
                    if (n.size() > 1) out.shin = n[1].get_value<std::string>();
                    if (n.size() > 2) out.foot = n[2].get_value<std::string>();
                } else if (n.is_mapping()) {
                    if (n.contains("thigh")) out.thigh = n.at("thigh").get_value<std::string>();
                    if (n.contains("shin")) out.shin = n.at("shin").get_value<std::string>();
                    if (n.contains("foot")) out.foot = n.at("foot").get_value<std::string>();
                    if (n.contains("pole")) out.pole = parse_vec3(n.at("pole"), out.pole);
                    if (n.contains("front")) out.front = n.at("front").get_value<bool>();
                }
            };
            auto leg = [&](const char* key, FootIK::Leg& out) {
                if (node.contains(key)) parse_leg(node.at(key), out);
            };
            if (node.contains("pelvis")) ik->pelvis = node.at("pelvis").get_value<std::string>();
            leg("left", ik->left);
            leg("right", ik->right);
            if (node.contains("legs") && node.at("legs").is_sequence()) {
                for (const auto& n : node.at("legs")) {
                    FootIK::Leg l;
                    parse_leg(n, l);
                    ik->legs.push_back(l);
                }
            }
            if (node.contains("body_pitch")) ik->body_pitch = node.at("body_pitch").get_value<bool>();
            f("max_body_pitch", ik->max_body_pitch);
            f("ray_up", ik->ray_up);
            f("ray_down", ik->ray_down);
            f("max_pelvis_drop", ik->max_pelvis_drop);
            if (node.contains("align_feet")) ik->align_feet = node.at("align_feet").get_value<bool>();
            f("max_foot_angle", ik->max_foot_angle);
            f("blend_speed", ik->blend_speed);
            f("weight", ik->weight);
            if (node.contains("layer_mask")) ik->layer_mask = static_cast<uint32_t>(node.at("layer_mask").get_value<int64_t>());
            if (node.contains("mode")) ik->mode = node.at("mode").get_value<std::string>();
        });

    SceneLoader::register_component_parser("KinematicMover",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* km = obj.add_component<KinematicMover>();

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                if (m == "orbit" || m == "Orbit") km->mode = KinematicMoverMode::Orbit;
                else if (m == "spin" || m == "Spin") km->mode = KinematicMoverMode::Spin;
                else km->mode = KinematicMoverMode::PingPong;
            }
            if (node.contains("axis")) km->axis = parse_vec3(node.at("axis"), km->axis);
            if (node.contains("distance")) km->distance = node.at("distance").get_value<float>();
            if (node.contains("speed")) km->speed = node.at("speed").get_value<float>();
            if (node.contains("orbit_center")) km->orbit_center = parse_vec3(node.at("orbit_center"), km->orbit_center);
            if (node.contains("orbit_radius")) km->orbit_radius = node.at("orbit_radius").get_value<float>();
            if (node.contains("spin_axis")) km->spin_axis = parse_vec3(node.at("spin_axis"), km->spin_axis);
            if (node.contains("spin_speed")) km->spin_speed = node.at("spin_speed").get_value<float>();
        });

    // Demo driver for the world-space UI canvas: owns a coopa::stat::Resource and binds it to
    // a ProgressBar in its own subtree at start(). See toyengine/scene/health_driver.h.
    SceneLoader::register_component_parser("HealthDriver",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* hd = obj.add_component<HealthDriver>();

            if (node.contains("bar_object")) hd->bar_object = node.at("bar_object").get_value<std::string>();
            if (node.contains("max_health")) hd->max_health = node.at("max_health").get_value<float>();
            if (node.contains("start_health")) hd->start_health = node.at("start_health").get_value<float>();
            if (node.contains("damage_per_second")) hd->damage_per_second = node.at("damage_per_second").get_value<float>();
            if (node.contains("regen_per_second")) hd->regen_per_second = node.at("regen_per_second").get_value<float>();
            if (node.contains("turnaround_fraction")) hd->turnaround_fraction = node.at("turnaround_fraction").get_value<float>();
        });

    // Free 3D movement for an object that is not a physics body -- typically an invisible
    // marker a camera tracks and the lens focuses on. Input is pushed in by
    // Engine::drive_free_movers_(), never read here; see the class doc.
    SceneLoader::register_component_parser("FreeMover",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* fm = obj.add_component<FreeMover>();
            if (node.contains("move_speed")) fm->move_speed = node.at("move_speed").get_value<float>();
            if (node.contains("smoothing"))  fm->smoothing  = node.at("smoothing").get_value<float>();
        });

    // Input-driven horizontal motion for a kinematic Rigidbody. The input itself is pushed in by
    // Engine::drive_kinematic_controllers_(), never read here -- see the class doc.
    SceneLoader::register_component_parser("KinematicController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* kc = obj.add_component<KinematicController>();
            if (node.contains("move_speed")) kc->move_speed = node.at("move_speed").get_value<float>();
            if (node.contains("smoothing")) kc->smoothing = node.at("smoothing").get_value<float>();
            if (node.contains("lock_height")) kc->lock_height = node.at("lock_height").get_value<bool>();
        });

    // Makes a sibling Rigidbody float -- see toyengine/water/buoyancy.h. Everything per-substep
    // happens in toy::water::WaterSystem; this is configuration only.
    SceneLoader::register_component_parser("Buoyancy",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* b = obj.add_component<water::Buoyancy>();
            if (node.contains("subdivisions"))   b->subdivisions   = node.at("subdivisions").get_value<int>();
            if (node.contains("volume"))         b->volume         = node.at("volume").get_value<float>();
            if (node.contains("linear_drag"))    b->linear_drag    = node.at("linear_drag").get_value<float>();
            if (node.contains("angular_drag"))   b->angular_drag   = node.at("angular_drag").get_value<float>();
            if (node.contains("form_drag"))      b->form_drag      = node.at("form_drag").get_value<float>();
            if (node.contains("buoyancy_scale")) b->buoyancy_scale = node.at("buoyancy_scale").get_value<float>();
            if (node.contains("pontoons")) {
                // [{x, y, z, radius}] in the owner's unscaled local frame.
                for (const auto& p : node.at("pontoons")) {
                    water::PontoonDesc d;
                    d.position = parse_vec3(p, glm::vec3(0.0f));
                    if (p.contains("radius")) d.radius = p.at("radius").get_value<float>();
                    b->pontoons.push_back(d);
                }
            }
        });

    // Jointed physics bones on a rig root (toyengine/scene/ragdoll.h). Each `bones:` entry is flat
    // (so the editor's item list can show it): {bone, shape: capsule|box|sphere, radius, height,
    // direction: x|y|z, size, center, mass, joint: cone_twist|hinge|ball, anchor, axis, swing,
    // twist_min, twist_max, limit_min, limit_max} -- angles in degrees.
    SceneLoader::register_component_parser("Ragdoll",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* r = obj.add_component<Ragdoll>();
            auto f = [&](const fkyaml::node& n, const char* key, float& out) {
                if (n.contains(key)) out = n.at(key).get_value<float>();
            };
            auto b = [&](const char* key, bool& out) {
                if (node.contains(key)) out = node.at(key).get_value<bool>();
            };
            auto axis_index = [](const std::string& s) {
                return (s == "x" || s == "X") ? 0 : (s == "y" || s == "Y") ? 1 : 2;
            };
            if (node.contains("mode")) r->start_ragdoll = node.at("mode").get_value<std::string>() == "ragdoll";
            b("auto_generate", r->auto_generate);
            b("collide_connected", r->collide_connected);
            b("link_character", r->link_character);
            b("reposition_root", r->reposition_root);
            b("input_toggle", r->input_toggle);
            f(node, "mass", r->mass);
            f(node, "drag", r->drag);
            f(node, "angular_drag", r->angular_drag);
            f(node, "blend_time", r->blend_time);
            f(node, "rest_speed", r->rest_speed);
            f(node, "rest_spin", r->rest_spin);
            f(node, "rest_time", r->rest_time);
            if (node.contains("layer")) r->layer = static_cast<uint32_t>(node.at("layer").get_value<int64_t>());
            if (node.contains("recover_state")) r->recover_state = node.at("recover_state").get_value<std::string>();
            if (node.contains("start_impulse")) r->start_impulse = parse_vec3(node.at("start_impulse"), r->start_impulse);
            if (node.contains("toggle_impulse")) r->toggle_impulse = parse_vec3(node.at("toggle_impulse"), r->toggle_impulse);
            if (!node.contains("bones") || !node.at("bones").is_sequence()) return;
            for (const auto& bn : node.at("bones")) {
                RagdollBone bone;
                if (bn.contains("bone")) bone.bone = bn.at("bone").get_value<std::string>();
                if (bn.contains("shape")) {
                    const std::string shape = bn.at("shape").get_value<std::string>();
                    bone.shape = shape == "box" ? RagdollBone::Shape::Box
                               : shape == "sphere" ? RagdollBone::Shape::Sphere
                                                   : RagdollBone::Shape::Capsule;
                }
                f(bn, "radius", bone.radius);
                f(bn, "height", bone.height);
                f(bn, "mass", bone.mass);
                if (bn.contains("direction")) bone.direction = axis_index(bn.at("direction").get_value<std::string>());
                if (bn.contains("size")) bone.size = parse_vec3(bn.at("size"), bone.size);
                if (bn.contains("center")) bone.center = parse_vec3(bn.at("center"), bone.center);
                if (bn.contains("joint")) {
                    const std::string type = bn.at("joint").get_value<std::string>();
                    bone.joint = type == "hinge" ? RagdollBone::Joint::Hinge
                               : type == "ball" ? RagdollBone::Joint::Ball
                                                : RagdollBone::Joint::ConeTwist;
                }
                if (bn.contains("anchor")) bone.anchor = parse_vec3(bn.at("anchor"), bone.anchor);
                if (bn.contains("axis")) bone.axis = parse_vec3(bn.at("axis"), bone.axis);
                f(bn, "swing", bone.swing_deg);
                f(bn, "twist_min", bone.twist_min_deg);
                f(bn, "twist_max", bone.twist_max_deg);
                f(bn, "limit_min", bone.hinge_min_deg);
                f(bn, "limit_max", bone.hinge_max_deg);
                r->bones.push_back(bone);
            }
        });
}

void register_scene_components(coopa::gfx::core::Device& device,
                                      coopa::gfx::memory::Allocator& allocator,
                                      coopa::asset::AssetManager& assets,
                                      uint32_t frames_in_flight) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    register_scene_components();

    // No fields of its own: everything it draws comes from the sibling Cloth and MeshRenderer.
    SceneLoader::register_component_parser("ClothRenderer",
        [&device, &allocator, &assets, frames_in_flight](
            const fkyaml::node&, SceneObject& obj, const SceneLoader::ParseContext&) {
            obj.add_component<ClothRenderer>(device, allocator, assets, frames_in_flight);
        });

    // `TileTerrain`: the streamed mapcoopa tile world (toyengine/world/); its old name, `Terrain`,
    // still loads (registered under both below). The component is configuration plus state;
    // every per-frame decision lives in toy::world::TerrainSystem, which toy::core::Engine
    // installs. Only `assets` is captured -- the chunk meshes this eventually builds are created
    // by that system, which holds the device and allocator itself.
    auto parse_tile_terrain =
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* terrain = obj.add_component<world::TerrainComponent>();
            world::TerrainParams& params = terrain->params;

            // --- The world to generate ---
            if (node.contains("seed"))      terrain->seed      = node.at("seed").get_value<int>();
            if (node.contains("grid_size")) terrain->grid_size = node.at("grid_size").get_value<int>();
            if (node.contains("sea_level")) terrain->sea_level = node.at("sea_level").get_value<double>();
            if (node.contains("terrain_roughness")) {
                terrain->terrain_roughness = node.at("terrain_roughness").get_value<double>();
            }
            if (node.contains("river_count")) {
                terrain->river_count = node.at("river_count").get_value<int>();
            }
            if (node.contains("shape")) terrain->shape = node.at("shape").get_value<std::string>();
            if (node.contains("continent_count")) {
                terrain->continent_count = node.at("continent_count").get_value<int>();
            }
            if (node.contains("continent_size_m")) {
                terrain->continent_size_m = node.at("continent_size_m").get_value<double>();
            }
            if (node.contains("irregularity")) terrain->irregularity = node.at("irregularity").get_value<double>();
            if (node.contains("coast_detail")) terrain->coast_detail = node.at("coast_detail").get_value<double>();
            if (node.contains("temperature_offset")) {
                terrain->temperature_offset = node.at("temperature_offset").get_value<double>();
            }

            // --- How it is tiled ---
            if (node.contains("tiles_per_grid_unit")) {
                params.tiles_per_grid_unit = node.at("tiles_per_grid_unit").get_value<std::int32_t>();
            }
            if (node.contains("tile_size"))    params.tile_size    = node.at("tile_size").get_value<float>();
            if (node.contains("height_step"))  params.height_step  = node.at("height_step").get_value<float>();
            if (node.contains("height_scale")) params.height_scale = node.at("height_scale").get_value<float>();
            if (node.contains("chunk_size")) {
                params.chunk_size = node.at("chunk_size").get_value<std::int32_t>();
            }
            if (node.contains("view_radius")) {
                params.view_radius = node.at("view_radius").get_value<std::int32_t>();
            }
            if (node.contains("max_wall_steps")) {
                params.max_wall_steps = node.at("max_wall_steps").get_value<std::int32_t>();
            }
            if (node.contains("soil_depth_steps")) {
                params.soil_depth_steps = node.at("soil_depth_steps").get_value<std::int32_t>();
            }
            if (node.contains("max_chunk_jobs_per_frame")) {
                terrain->max_chunk_jobs_per_frame = node.at("max_chunk_jobs_per_frame").get_value<int>();
            }

            // Shared with the "MeshRenderer" parser rather than reimplemented, so a terrain's
            // atlas gets the same sRGB colour-space declaration and async load path every other
            // textured material in the engine gets -- see gfxcoopa's parse_pbr_material_().
            //
            // The chunks' shader reads each surface kind's colour from the palette atlas, so a
            // material with no `texture_albedo` (a freshly added TileTerrain, say) gets the stock
            // one rather than drawing the whole world white. A material asset reference is
            // taken as written.
            {
                fkyaml::node material = node.contains("material") ? node.at("material") : fkyaml::node::mapping();
                if (material.is_mapping() && !material.contains("texture_albedo")) {
                    material["texture_albedo"] = fkyaml::node(std::string("textures/terrain_atlas.png"));
                    if (!material.contains("texture_metallic_roughness")) {
                        material["texture_metallic_roughness"] = fkyaml::node(std::string("textures/terrain_atlas_mr.png"));
                    }
                }
                coopa::gfx::engine::components::parse_material_value_(material, terrain->material, assets, ctx);
            }

            // --- The stored tiles ---
            // `tiles:` names the terrain's .tiles file, scene-relative, default terrain/<object>.tiles.
            // Resolved for reading when it exists, else to where a save will create it; the store
            // hands back the same live tiles on every re-parse (unsaved edits included), and an
            // empty grid -- baked from the generator keys on first use -- when there is no file.
            terrain->tiles_file = node.contains("tiles") ? node.at("tiles").get_value<std::string>()
                                                         : "terrain/" + obj.name() + ".tiles";
            {
                std::string path = assets.source().resolve(terrain->tiles_file, ctx.base_dir());
                if (!std::filesystem::exists(path)) {
                    path = std::filesystem::path(terrain->tiles_file).is_absolute()
                               ? terrain->tiles_file
                               : (std::filesystem::path(ctx.base_dir()) / terrain->tiles_file).string();
                }
                terrain->tiles = world::TileMapStore::instance().acquire(path);
            }

            if (node.contains("collision")) terrain->collision = node.at("collision").get_value<bool>();
            if (node.contains("physics_material")) {
                terrain->physics_material = node.at("physics_material").get_value<std::string>();
            }

            // --- The tile sets (toyengine/world/tile_topology.h) ---
            // `tile_set: objects/terrain/tileset_round` is the template every column is built from;
            // `kind_tile_sets: {stone: objects/terrain/tileset_rock}` overrides it per surface kind.
            // A tile set is an object asset whose children are the pieces, each found by its NAME
            // (tile_piece_name()) and taken from that child's MeshRenderer mesh_path; a piece with
            // no child falls back like a missing file (TileMeshLibrary::finish_styles()).
            using coopa::gfx::engine::data::SkinnedMeshSource;
            auto load_piece = [&assets, &ctx](const std::string& key) {
                return assets.load_async<SkinnedMeshSource>("meshes/" + key + ".yaml", ctx.base_dir());
            };
            // Adds one style (a distinct tile set) and kicks off its pieces' loads; returns its index.
            // `value` is a tile-set object path, or -- the old `styles:` form -- a mesh prefix the
            // pieces load from (meshes/<prefix>_<piece>.yaml).
            auto add_style = [&](const std::string& name, const std::string& value) {
                world::TerrainComponent::StyleEntry entry;
                entry.name   = name;
                entry.prefix = value;
                const std::size_t index = terrain->styles.size();
                if (value.rfind("objects/", 0) == 0) {
                    std::string rel = value;
                    if (rel.size() < 5 || rel.compare(rel.size() - 5, 5, ".yaml") != 0) rel += ".yaml";
                    try {
                        const fkyaml::node doc =
                            coopa::yaml::load_document(assets.source().resolve(rel, ctx.base_dir()));
                        const fkyaml::node& obj = doc.at("object");
                        if (obj.contains("round_kind_corners")) {
                            entry.round_kind_corners = obj.at("round_kind_corners").get_value<bool>();
                        }
                        if (obj.contains("children") && obj.at("children").is_sequence()) {
                            for (const auto& child : obj.at("children")) {
                                if (!child.contains("name") || !child.contains("components")) continue;
                                const std::string child_name = child.at("name").get_value<std::string>();
                                for (std::size_t p = 0; p < world::k_tile_piece_count; ++p) {
                                    const auto piece = static_cast<world::TilePiece>(p);
                                    if (child_name != world::tile_piece_name(piece)) continue;
                                    for (const auto& comp : child.at("components")) {
                                        if (!comp.contains("type") || !comp.contains("mesh_path") ||
                                            comp.at("type").get_value<std::string>() != "MeshRenderer") continue;
                                        terrain->set_style_piece_source(
                                            index, piece, load_piece(comp.at("mesh_path").get_value<std::string>()));
                                    }
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[TileTerrain] tile set '" << value << "' could not be read: " << e.what() << "\n";
                    }
                } else {
                    for (std::size_t p = 0; p < world::k_tile_piece_count; ++p) {
                        const auto piece = static_cast<world::TilePiece>(p);
                        terrain->set_style_piece_source(index, piece,
                                                        load_piece(value + "_" + world::tile_piece_name(piece)));
                    }
                }
                terrain->styles.push_back(std::move(entry));
                return static_cast<int>(index);
            };
            auto style_named = [terrain](const std::string& name) {
                for (std::size_t i = 0; i < terrain->styles.size(); ++i) {
                    if (terrain->styles[i].name == name) return static_cast<int>(i);
                }
                return -1;
            };

            if (node.contains("styles") && node.at("styles").is_mapping() && !node.contains("tile_set")) {
                // The older form: `styles: {round: objects/tileset_round}` names each set, and
                // `kind_styles: {stone: rock, default: round}` picks one per kind. Without a
                // `default`, unlisted kinds take the first style parsed.
                for (auto item : node.at("styles").map_items()) {
                    add_style(item.key().get_value<std::string>(), item.value().get_value<std::string>());
                }
                if (node.contains("kind_styles") && node.at("kind_styles").is_mapping()) {
                    for (auto item : node.at("kind_styles").map_items()) {
                        const std::string kind_name = item.key().get_value<std::string>();
                        const int style = style_named(item.value().get_value<std::string>());
                        if (style < 0) continue;
                        world::TileKind kind;
                        if (kind_name == "default") {
                            terrain->default_style = style;
                        } else if (world::tile_kind_from_name(kind_name.c_str(), kind)) {
                            terrain->kind_styles[static_cast<std::size_t>(kind)] = style;
                        }
                    }
                }
            } else {
                if (node.contains("tile_set")) {
                    const std::string path = node.at("tile_set").get_value<std::string>();
                    if (!path.empty()) terrain->tile_set = path;
                } else if (node.contains("side_mesh") || node.contains("sides")) {
                    // The retired voxel mesher's keys: the voxel look is now a tile set.
                    std::cerr << "[TileTerrain] `side_mesh` / `sides` are no longer used -- building from '"
                              << terrain->tile_set << "'; set `tile_set:` instead\n";
                }
                terrain->default_style = add_style(terrain->tile_set, terrain->tile_set);
                if (node.contains("kind_tile_sets") && node.at("kind_tile_sets").is_mapping()) {
                    for (auto item : node.at("kind_tile_sets").map_items()) {
                        const std::string kind_name = item.key().get_value<std::string>();
                        const std::string path = item.value().get_value<std::string>();
                        world::TileKind kind;
                        if (path.empty() || !world::tile_kind_from_name(kind_name.c_str(), kind)) continue;
                        terrain->kind_tile_sets[static_cast<std::size_t>(kind)] = path;
                        int style = style_named(path);
                        if (style < 0) style = add_style(path, path);
                        terrain->kind_styles[static_cast<std::size_t>(kind)] = style;
                    }
                }
            }
        };
    SceneLoader::register_component_parser("TileTerrain", parse_tile_terrain);
    SceneLoader::register_component_parser("Terrain", parse_tile_terrain);


    // The sculptable heightmap terrain -- see toyengine/world/heightmap_terrain.h. Drawn, collided
    // and streamed by toy::world::HeightmapTerrainSystem, which Engine installs.
    world::register_heightmap_terrain_component(assets);

    // A lake, ocean or river -- see toyengine/water/water_body.h. Baked and published (mesh +
    // material params on the sibling MeshRenderer) by toy::water::WaterSystem, which Engine
    // installs. `mesh_path` loads CPU-side only (a SkinnedMeshSource, since the bake needs the
    // vertices), so the sibling MeshRenderer must not name the same mesh itself.
    SceneLoader::register_component_parser("WaterBody",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* w = obj.add_component<water::WaterBody>();
            auto f = [&node](const char* key, float& out) {
                if (node.contains(key)) out = node.at(key).get_value<float>();
            };
            if (node.contains("mode")) {
                std::string mode = node.at("mode").get_value<std::string>();
                for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (mode == "flowing" || mode == "river") w->mode = water::WaterMode::Flowing;
                else if (mode == "planar" || mode == "static" || mode == "lake") w->mode = water::WaterMode::Planar;
                else throw std::runtime_error("[WaterBody] unknown mode '" + mode + "' (planar | flowing)");
            }
            if (node.contains("mesh_path")) w->mesh_path = node.at("mesh_path").get_value<std::string>();
            if (node.contains("size")) {
                const auto& sz = node.at("size");
                if (sz.contains("x")) w->size.x = sz.at("x").get_value<float>();
                if (sz.contains("y")) w->size.y = sz.at("y").get_value<float>();
            }
            if (node.contains("resolution")) w->resolution = node.at("resolution").get_value<int>();
            f("tile_size", w->tile_size);

            f("wave_amplitude", w->waves.amplitude);
            f("wave_length", w->waves.wavelength);
            f("wave_steepness", w->waves.steepness);
            f("wind_response", w->wind_response);
            if (node.contains("wave_direction")) {
                w->waves.direction = glm::radians(node.at("wave_direction").get_value<float>());
            }

            f("flow_speed", w->flow_speed);
            f("flow_min_speed", w->flow_min_speed);
            f("flow_slope_gain", w->flow_slope_gain);
            f("obstacle_radius", w->obstacle_radius);
            f("wake_length", w->wake_length);

            if (node.contains("foam_color")) w->foam_color = parse_rgb(node.at("foam_color"), w->foam_color);
            f("foam_amount", w->foam_amount);
            f("shore_foam_depth", w->shore_foam_depth);
            f("edge_fade_depth", w->edge_fade_depth);
            f("ripple_strength", w->ripple_strength);
            f("ripple_scale", w->ripple_scale);
            f("clarity", w->clarity);

            if (node.contains("underwater_color")) w->underwater_color = parse_rgb(node.at("underwater_color"), w->underwater_color);
            f("underwater_visibility", w->underwater_visibility);
            if (node.contains("underwater_absorption")) {
                w->underwater_absorption = parse_rgb(node.at("underwater_absorption"), w->underwater_absorption);
            }
            f("caustics", w->caustics);

            f("density", w->density);
            f("max_depth", w->max_depth);

            if (!w->mesh_path.empty()) {
                w->source = assets.load_async<coopa::gfx::engine::data::SkinnedMeshSource>(
                    "meshes/" + w->mesh_path + ".yaml", ctx.base_dir());
            }
        });

    // CPU-skins a bind-pose mesh against animated bone SceneObjects every frame -- see
    // skinned_mesh_renderer.h's file doc for why toyengine does this on the CPU rather than
    // via GPU vertex skinning. `mesh_path` is resolved and load-kicked off here (like
    // register_render_components()'s "MeshRenderer" parser resolves its own), since only the
    // parser has ctx.scene_dir; `bones:` name SceneObject paths in the mesh's joint-index order.
    SceneLoader::register_component_parser("SkinnedMeshRenderer",
        [&device, &allocator, &assets, frames_in_flight](
            const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* smr = obj.add_component<SkinnedMeshRenderer>(device, allocator, assets, frames_in_flight);

            if (node.contains("mesh_path")) {
                std::string mesh_path_key = node.at("mesh_path").get_value<std::string>();
                if (!mesh_path_key.empty()) {
                    std::string virtual_path = "meshes/" + mesh_path_key + ".yaml";
                    smr->set_source(
                        assets.load_async<coopa::gfx::engine::data::SkinnedMeshSource>(virtual_path, ctx.base_dir()));
                }
            }

            if (node.contains("bones")) {
                std::vector<std::string> bones;
                for (const auto& b : node.at("bones")) bones.push_back(b.get_value<std::string>());
                smr->set_bones(std::move(bones));
            }
            if (node.contains("rig")) smr->set_rig(node.at("rig").get_value<std::string>());
        });
}

} // namespace scene
} // namespace toy
