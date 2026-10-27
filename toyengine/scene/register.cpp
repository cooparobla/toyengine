#include <toyengine/scene/register.h>

#include <toyengine/core/yaml_read.h>

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
#include <toyengine/audio/audio_demo_panel.h>
#include <toyengine/ui/demo_hud.h>
#include <toyengine/ui/panel_screen.h>
#include <toyengine/ui/screen_hint.h>
#include <toyengine/ui/ui_tunables.h>
#include <toyengine/ui/world_label.h>
#include <toyengine/scene/health_driver.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>
#include <toyengine/scene/mount.h>
#include <toyengine/scene/ragdoll.h>
#include <toyengine/scene/rider.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/scene/destructible.h>
#include <toyengine/scene/skinned_mesh_renderer.h>
#include <toyengine/scene/terrain_digger.h>
#include <toyengine/scene/turntable.h>
#include <toyengine/scene/view_blocker.h>
#include <toyengine/scene/xray_markers.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_yaml.h>
#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/terrain_yaml.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/mapgen/terrain_generator.h>
#include <toyengine/world/tile_map_data.h>

namespace toy {
namespace scene {

// The present-if-set key reads every parser below uses (toyengine/core/yaml_read.h).
using core::yaml::read;
using core::yaml::read_rgb;
using core::yaml::num;

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

            read(node, "distance", cc->distance);
            read(node, "yaw_deg", cc->yaw_deg);
            read(node, "pitch_deg", cc->pitch_deg);

            read(node, "min_pitch_deg", cc->min_pitch_deg);
            read(node, "max_pitch_deg", cc->max_pitch_deg);
            read(node, "min_distance", cc->min_distance);
            read(node, "max_distance", cc->max_distance);

            if (node.contains("mouse_sensitivity")) {
                cc->mouse_sensitivity = node.at("mouse_sensitivity").get_value<float>();
            }
            read(node, "invert_x", cc->invert_x);
            read(node, "invert_y", cc->invert_y);
            read(node, "zoom_speed", cc->zoom_speed);
            if (node.contains("look_button")) {
                cc->look_button = node.at("look_button").get_value<std::string>();
            }
            if (node.contains("capture_cursor")) {
                cc->capture_cursor = node.at("capture_cursor").get_value<bool>();
            }

            if (node.contains("auto_rotate_deg_per_sec")) {
                cc->auto_rotate_deg_per_sec = node.at("auto_rotate_deg_per_sec").get_value<float>();
            }
            if (node.contains("key_orbit_speed_deg_per_sec")) {
                cc->key_orbit_speed_deg_per_sec = node.at("key_orbit_speed_deg_per_sec").get_value<float>();
            }
            if (node.contains("move_speed")) {
                cc->move_speed = node.at("move_speed").get_value<float>();
            }
            if (node.contains("look_speed_deg_per_sec")) {
                cc->look_speed_deg_per_sec = node.at("look_speed_deg_per_sec").get_value<float>();
            }
            read(node, "collide", cc->collide);
            read(node, "collision_radius", cc->collision_radius);
            read(node, "collision_in_speed", cc->collision_in_speed);
            read(node, "collision_out_speed", cc->collision_out_speed);
            read(node, "eye_height", cc->eye_height);
            read(node, "first_person_pitch_limit", cc->first_person_pitch_limit);
        });

    // A model turned in front of a fixed camera (toyengine/scene/turntable.h). Input is pushed
    // in by Engine::drive_turntables_(); it owns the owner's rotation from start() on.
    SceneLoader::register_component_parser("Turntable",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* tt = obj.add_component<Turntable>();
            read(node, "sensitivity", tt->sensitivity);
            read(node, "drag_button", tt->drag_button);
            if (node.contains("axis")) tt->axis = parse_vec3(node.at("axis"), tt->axis);
            read(node, "idle_spin_deg_per_sec", tt->idle_spin_deg_per_sec);
            read(node, "damping", tt->damping);
            read(node, "min_tilt_deg", tt->min_tilt_deg);
            read(node, "max_tilt_deg", tt->max_tilt_deg);
            read(node, "max_spin_deg_per_sec", tt->max_spin_deg_per_sec);
            read(node, "invert_x", tt->invert_x);
            read(node, "invert_y", tt->invert_y);
        });

    // A breakable mesh (toyengine/scene/destructible.h). DestructionSystem (order 70) does the
    // fracturing and the chunk spawning; this is the settings it reads.
    SceneLoader::register_component_parser("Destructible",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<Destructible>();
            read(node, "pieces", d->pieces);
            read(node, "seed", d->seed);
            read(node, "max_chunks", d->max_chunks);
            read(node, "max_boxes", d->max_boxes);
            read(node, "prefracture", d->prefracture);
            if (node.contains("chunk_physics_material")) {
                d->chunk_physics_material = node.at("chunk_physics_material").get_value<std::string>();
            }
            read(node, "irregularity", d->irregularity);
            read(node, "impact_impulse", d->impact_impulse);
            read(node, "scatter", d->scatter);
            read(node, "impact_transfer", d->impact_transfer);
            read(node, "chunk_lifetime", d->chunk_lifetime);
            read(node, "chunk_fade", d->chunk_fade);
            read(node, "chunk_density", d->chunk_density);
            read(node, "collider_fill", d->collider_fill);
        });

    // Digging the terrain with the mouse (toyengine/scene/terrain_digger.h). Input is pushed in
    // by Engine::drive_terrain_diggers_(); it edits the scene's first TileTerrain.
    SceneLoader::register_component_parser("TerrainDigger",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<TerrainDigger>();
            read(node, "dig_button", d->dig_button);
            read(node, "raise_key", d->raise_key);
            read(node, "radius", d->radius);
            read(node, "steps", d->steps);
            read(node, "repeat_seconds", d->repeat_seconds);
            read(node, "reach", d->reach);
        });

    // A walking capsule character (toyengine/scene/character_controller.h). Input is pushed in by
    // Engine::drive_character_controllers_(); it adds its own kinematic Rigidbody + CapsuleCollider.
    SceneLoader::register_component_parser("CharacterController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<CharacterController>();
            read(node, "radius", cc->radius);
            read(node, "height", cc->height);
            read(node, "step_height", cc->step_height);
            read(node, "slope_limit", cc->slope_limit);
            read(node, "skin", cc->skin);
            read(node, "snap_distance", cc->snap_distance);
            read(node, "step_smoothing", cc->step_smoothing);
            read(node, "move_speed", cc->move_speed);
            read(node, "sprint_multiplier", cc->sprint_multiplier);
            read(node, "acceleration", cc->acceleration);
            read(node, "air_control", cc->air_control);
            read(node, "gravity_scale", cc->gravity_scale);
            read(node, "jump_height", cc->jump_height);
            read(node, "coyote_time", cc->coyote_time);
            read(node, "jump_buffer", cc->jump_buffer);
            read(node, "face_movement", cc->face_movement);
            read(node, "turn_speed", cc->turn_speed);
            read(node, "push_dynamic_bodies", cc->push_dynamic_bodies);
            read(node, "push_strength", cc->push_strength);
            read(node, "use_root_motion", cc->use_root_motion);
            read(node, "player_input", cc->player_input);
        });

    // A rideable animal (toyengine/scene/mount.h): steers its own CharacterController from the
    // rider's input, plays its gait clips, and names the sockets a Rider sits on / reaches for.
    SceneLoader::register_component_parser("Mount",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* m = obj.add_component<Mount>();
            read(node, "walk_speed", m->walk_speed);
            read(node, "trot_speed", m->trot_speed);
            read(node, "gallop_speed", m->gallop_speed);
            read(node, "acceleration", m->acceleration);
            read(node, "braking", m->braking);
            read(node, "turn_rate", m->turn_rate);
            read(node, "gallop_turn_rate", m->gallop_turn_rate);
            read(node, "mount_max_speed", m->mount_max_speed);
            read(node, "idle_state", m->idle_state);
            read(node, "walk_state", m->walk_state);
            read(node, "trot_state", m->trot_state);
            read(node, "gallop_state", m->gallop_state);
            read(node, "crossfade", m->crossfade);
            read(node, "min_anim_speed", m->min_anim_speed);
            read(node, "max_anim_speed", m->max_anim_speed);
            read(node, "saddle", m->saddle);
            read(node, "stirrup_l", m->stirrup_l);
            read(node, "stirrup_r", m->stirrup_r);
            read(node, "rein_l", m->rein_l);
            read(node, "rein_r", m->rein_r);
            read(node, "mount_point_l", m->mount_point_l);
            read(node, "mount_point_r", m->mount_point_r);
            read(node, "camera_offset_z", m->camera_offset_z);
            read(node, "camera_distance", m->camera_distance);
        });

    // Gets the player on / off a Mount (toyengine/scene/rider.h); `interact` (F) is pushed by
    // Engine::drive_character_controllers_().
    SceneLoader::register_component_parser("Rider",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* r = obj.add_component<Rider>();
            read(node, "mount_radius", r->mount_radius);
            read(node, "mount_time", r->mount_time);
            read(node, "dismount_time", r->dismount_time);
            read(node, "ride_state", r->ride_state);
            read(node, "crossfade", r->crossfade);
            if (node.contains("seat_offset")) r->seat_offset = parse_vec3(node.at("seat_offset"), r->seat_offset);
            read(node, "knee_spread", r->knee_spread);
            read(node, "lean", r->lean);
            read(node, "leg_weight", r->leg_weight);
            read(node, "arm_weight", r->arm_weight);
            read(node, "priority", r->priority);
            read(node, "mount_on_start", r->mount_on_start);
        });

    // A trigger box that loads another scene when the player walks in (toyengine/scene/scene_link.h).
    // A relative target_scene resolves against the declaring file first (a sibling scene), then
    // the asset roots; anything still unresolved goes to the engine's scene lookup by name.
    SceneLoader::register_component_parser("SceneLink",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* link = obj.add_component<SceneLink>();
            read(node, "target_scene", link->target_scene);
            if (!link->target_scene.empty()) link->target_scene = ctx.resolve(link->target_scene);
            read(node, "transition", link->transition);
            read(node, "loading_screen", link->loading_screen);
            read(node, "spawn_point", link->spawn_point);
            if (node.contains("color")) link->color = parse_rgb(node.at("color"), link->color);
            read(node, "fade_time", link->fade_time);
            read(node, "min_display_time", link->min_display_time);
            if (node.contains("size")) link->size = parse_vec3(node.at("size"), link->size);
        });

    // Demo: crossfades the Animator between idle/walk/run/jump from the CharacterController.
    SceneLoader::register_component_parser("CharacterAnimDriver",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<CharacterAnimDriver>();
            read(node, "idle_state", d->idle_state);
            read(node, "walk_state", d->walk_state);
            read(node, "run_state", d->run_state);
            read(node, "jump_state", d->jump_state);
            read(node, "walk_speed", d->walk_speed);
            read(node, "run_speed", d->run_speed);
            read(node, "crossfade", d->crossfade);
            read(node, "air_delay", d->air_delay);
        });

    // Ground-adapting legs (toyengine/scene/foot_ik.h), solved by coopa::anim::IkSystem.
    SceneLoader::register_component_parser("FootIK",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<FootIK>();
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
            read(node, "pelvis", ik->pelvis);
            leg("left", ik->left);
            leg("right", ik->right);
            if (node.contains("legs") && node.at("legs").is_sequence()) {
                for (const auto& n : node.at("legs")) {
                    FootIK::Leg l;
                    parse_leg(n, l);
                    ik->legs.push_back(l);
                }
            }
            read(node, "body_pitch", ik->body_pitch);
            read(node, "max_body_pitch", ik->max_body_pitch);
            read(node, "ray_up", ik->ray_up);
            read(node, "ray_down", ik->ray_down);
            read(node, "max_pelvis_drop", ik->max_pelvis_drop);
            read(node, "align_feet", ik->align_feet);
            read(node, "max_foot_angle", ik->max_foot_angle);
            read(node, "blend_speed", ik->blend_speed);
            read(node, "weight", ik->weight);
            if (node.contains("layer_mask")) ik->layer_mask = static_cast<uint32_t>(node.at("layer_mask").get_value<int64_t>());
            read(node, "mode", ik->mode);
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
            read(node, "distance", km->distance);
            read(node, "speed", km->speed);
            if (node.contains("orbit_center")) km->orbit_center = parse_vec3(node.at("orbit_center"), km->orbit_center);
            read(node, "orbit_radius", km->orbit_radius);
            if (node.contains("spin_axis")) km->spin_axis = parse_vec3(node.at("spin_axis"), km->spin_axis);
            read(node, "spin_speed", km->spin_speed);
        });

    // Demo driver for the world-space UI canvas: owns a coopa::stat::Resource and binds it to
    // a ProgressBar in its own subtree at start(). See toyengine/scene/health_driver.h.
    // A plate in the world: a nameplate, a sign, a label on a machine.
    SceneLoader::register_component_parser("WorldLabel",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* w = obj.add_component<toy::ui::WorldLabel>();
            read(node, "title", w->title);
            read(node, "subtitle", w->subtitle);
            read(node, "bar_value", w->bar_value);
            read(node, "bar_max", w->bar_max);
            read(node, "bar_role", w->bar_role);
            read(node, "backdrop", w->backdrop);
            read(node, "theme_ref", w->theme_ref);
            read(node, "font_ref", w->font_ref);
            read(node, "world_size", w->world_size);
            read(node, "pixels_per_unit", w->pixels_per_unit);
            read(node, "world_text_supersample", w->world_text_supersample);
            read(node, "world_occlude", w->world_occlude);
            read(node, "tunables_ref", w->tunables_ref);
            read(node, "hidden", w->hidden);
            std::string billboard;
            if (read(node, "billboard", billboard) && billboard == "Transform") {
                w->world_billboard = coopa::ui::CanvasBillboard::Transform;
            }
        });

    // A screen authored as YAML (toyengine/ui/ui_panel.h): tabs, rows and buttons, bound to
    // UserSettings by default. No C++ per screen.
    SceneLoader::register_component_parser("PanelScreen",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* p = obj.add_component<toy::ui::PanelScreen>();
            read(node, "panel", p->panel);
            read(node, "theme_ref", p->theme_ref);
            read(node, "font_ref", p->font_ref);
            read(node, "sort_order", p->sort_order);
            read(node, "tunables_ref", p->tunables_ref);
            read(node, "ui_sounds", p->ui_sounds);
            read(node, "hidden", p->hidden);
        });

    // A line or two pinned to a corner: controls, a reminder.
    SceneLoader::register_component_parser("ScreenHint",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* h = obj.add_component<toy::ui::ScreenHint>();
            read(node, "title", h->title);
            if (node.contains("lines") && node.at("lines").is_sequence()) {
                for (const auto& l : node.at("lines")) h->lines.push_back(l.get_value<std::string>());
            }
            read(node, "offset", h->offset);
            read(node, "backdrop", h->backdrop);
            read(node, "theme_ref", h->theme_ref);
            read(node, "font_ref", h->font_ref);
            read(node, "sort_order", h->sort_order);
            read(node, "tunables_ref", h->tunables_ref);
            read(node, "hidden", h->hidden);
            std::string a;
            if (read(node, "anchor", a)) {
                coopa::ui::imm::Anchor parsed = h->anchor;
                if (toy::ui::parse_anchor(a, parsed)) h->anchor = parsed;
            }
        });

    // The stock screen-space HUD -- vitals, quest, log, hotbar, key hints.
    SceneLoader::register_component_parser("DemoHud",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* h = obj.add_component<toy::ui::DemoHud>();
            read(node, "health", h->health);
            read(node, "health_max", h->health_max);
            read(node, "stamina", h->stamina);
            read(node, "stamina_max", h->stamina_max);
            read(node, "mana", h->mana);
            read(node, "mana_max", h->mana_max);
            read(node, "quest_title", h->quest_title);
            read(node, "quest_step", h->quest_step);
            read(node, "hotbar_slots", h->hotbar_slots);
            read(node, "animate", h->animate);
            read(node, "theme_ref", h->theme_ref);
            read(node, "font_ref", h->font_ref);
            read(node, "sort_order", h->sort_order);
            read(node, "tunables_ref", h->tunables_ref);
            read(node, "ui_sounds", h->ui_sounds);
            read(node, "hidden", h->hidden);
        });

    // The audio demo's control panel: an immediate-mode screen, so the scene names the
    // component and the panel declares its own widgets per frame.
    SceneLoader::register_component_parser("AudioDemoPanel",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* p = obj.add_component<toy::audio::AudioDemoPanel>();
            read(node, "playlist_object", p->playlist_object);
            read(node, "fade_seconds", p->fade_seconds);
            read(node, "start_page", p->start_page);
            read(node, "theme_ref", p->theme_ref);
            read(node, "font_ref", p->font_ref);
            read(node, "sort_order", p->sort_order);
            read(node, "hidden", p->hidden);
        });

    SceneLoader::register_component_parser("HealthDriver",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* hd = obj.add_component<HealthDriver>();

            read(node, "label", hd->label);
            read(node, "max_health", hd->max_health);
            read(node, "start_health", hd->start_health);
            read(node, "damage_per_second", hd->damage_per_second);
            read(node, "regen_per_second", hd->regen_per_second);
            read(node, "turnaround_fraction", hd->turnaround_fraction);
            read(node, "show_heal_button", hd->show_heal_button);
            // The plate it draws itself on.
            read(node, "theme_ref", hd->theme_ref);
            read(node, "font_ref", hd->font_ref);
            read(node, "world_size", hd->world_size);
            read(node, "pixels_per_unit", hd->pixels_per_unit);
            read(node, "world_text_supersample", hd->world_text_supersample);
            read(node, "world_occlude", hd->world_occlude);
            read(node, "hidden", hd->hidden);
        });

    // Free 3D movement for an object that is not a physics body -- typically an invisible
    // marker a camera tracks and the lens focuses on. Input is pushed in by
    // Engine::drive_free_movers_(), never read here; see the class doc.
    SceneLoader::register_component_parser("FreeMover",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* fm = obj.add_component<FreeMover>();
            read(node, "move_speed", fm->move_speed);
            read(node, "smoothing", fm->smoothing);
        });

    // Input-driven horizontal motion for a kinematic Rigidbody. The input itself is pushed in by
    // Engine::drive_kinematic_controllers_(), never read here -- see the class doc.
    SceneLoader::register_component_parser("KinematicController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* kc = obj.add_component<KinematicController>();
            read(node, "move_speed", kc->move_speed);
            read(node, "smoothing", kc->smoothing);
            read(node, "lock_height", kc->lock_height);
        });

    // Makes a sibling Rigidbody float -- see toyengine/water/buoyancy.h. Everything per-substep
    // happens in toy::water::WaterSystem; this is configuration only.
    SceneLoader::register_component_parser("Buoyancy",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* b = obj.add_component<water::Buoyancy>();
            read(node, "subdivisions", b->subdivisions);
            read(node, "volume", b->volume);
            read(node, "linear_drag", b->linear_drag);
            read(node, "angular_drag", b->angular_drag);
            read(node, "form_drag", b->form_drag);
            read(node, "buoyancy_scale", b->buoyancy_scale);
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
            auto axis_index = [](const std::string& s) {
                return (s == "x" || s == "X") ? 0 : (s == "y" || s == "Y") ? 1 : 2;
            };
            if (node.contains("mode")) r->start_ragdoll = node.at("mode").get_value<std::string>() == "ragdoll";
            read(node, "auto_generate", r->auto_generate);
            read(node, "collide_connected", r->collide_connected);
            read(node, "link_character", r->link_character);
            read(node, "reposition_root", r->reposition_root);
            read(node, "input_toggle", r->input_toggle);
            f(node, "mass", r->mass);
            f(node, "drag", r->drag);
            f(node, "angular_drag", r->angular_drag);
            f(node, "blend_time", r->blend_time);
            f(node, "rest_travel", r->rest_travel);
            f(node, "rest_time", r->rest_time);
            f(node, "max_limp_speed", r->max_limp_speed);
            f(node, "max_limp_spin", r->max_limp_spin);
            if (node.contains("layer")) r->layer = static_cast<uint32_t>(node.at("layer").get_value<int64_t>());
            read(node, "recover_state", r->recover_state);
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

    // The two x-ray markers (toyengine/scene/xray_markers.h). Plain opt-in flags: the pass
    // only exists when the `xray_enabled` render setting built it, and reads nothing else.
    SceneLoader::register_component_parser("XraySubject",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* s = obj.add_component<XraySubject>();
            read(node, "enabled", s->enabled);
        });
    SceneLoader::register_component_parser("XrayOccluder",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* o = obj.add_component<XrayOccluder>();
            read(node, "enabled", o->enabled);
        });

    // A wall that erodes out of the way of the view (toyengine/scene/view_blocker.h).
    // Driven by install_view_blocker_system(); without that system it stays solid.
    SceneLoader::register_component_parser("ViewBlockerFade",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* f = obj.add_component<ViewBlockerFade>();
            read(node, "fade_target", f->fade_target);
            read(node, "fade_in_time", f->fade_in_time);
            read(node, "fade_out_time", f->fade_out_time);
            read(node, "cutoff_steps", f->cutoff_steps);
            read(node, "keep_shadow", f->keep_shadow);
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
            // One component type, two modes: this parser authors the tile one.
            terrain->mode = world::TerrainMode::Tiles;
            world::TerrainParams& params = terrain->params;

            // The surfaces it can wear. Left empty -- which is the usual case -- a tile world
            // uses the built-in set (builtin_tile_layers()), so an unlisted terrain is unchanged.
            world::parse_terrain_layers(node, ctx.base_dir(), assets, *terrain);

            // --- The world to generate. Generation is not the terrain's any more: a
            //     toy::mapgen::MapGeneratorComponent owns it and APPLIES it (toyengine/mapgen).
            //     A scene that writes `generator:` on its terrain -- which is every tile scene
            //     written so far, and a bare `TileTerrain` with no block at all -- gets one
            //     synthesised here, targeting this same object, so it keeps generating exactly
            //     as it did. `terrain->generator` stays as the copy the editor's World Generator
            //     reads back; the component beside it is what actually generates. ---
            terrain->generator = mapgen::generator_config_from_node(node);

            // --- How it is tiled ---
            read(node, "world_tiles", params.world_tiles);
            if (node.contains("tiles_per_grid_unit")) {
                params.tiles_per_grid_unit = node.at("tiles_per_grid_unit").get_value<std::int32_t>();
            }
            read(node, "tile_size", params.tile_size);
            read(node, "height_step", params.height_step);
            read(node, "height_scale", params.height_scale);
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
            read(node, "water_inset", params.water_inset);
            // A planet (toyengine/world/planet.h): six faces of tiles on a sphere.
            read(node, "planet", params.planet);
            read(node, "face_tiles", params.face_tiles);
            read(node, "gravity", terrain->gravity);
            if (params.planet) {
                params.max_merge = 1;   // every tile its own piece: merged quads would crack when bent
                params.face_tiles = std::max<std::int32_t>(params.chunk_size, params.face_tiles);
                params.face_tiles -= params.face_tiles % std::max<std::int32_t>(1, params.chunk_size);
            }
            read(node, "river_inset", params.river_inset);
            // `lods:` -- the coarser levels every chunk mesh is built with, coarsest last. An
            // empty list (or `lods: false`) turns the levels off and draws every chunk at full
            // density, whatever its size on screen.
            if (node.contains("lods")) {
                const fkyaml::node& l = node.at("lods");
                params.chunk_lods = {};
                if (l.is_sequence()) {
                    std::size_t i = 0;
                    for (const fkyaml::node& level : l) {
                        if (i >= world::TerrainParams::k_max_chunk_lods) break;
                        if (!level.is_mapping()) continue;
                        world::TerrainParams::ChunkLod& out = params.chunk_lods[i++];
                        if (level.contains("ratio")) out.ratio = level.at("ratio").get_value<float>();
                        if (level.contains("screen_size")) out.screen_size = level.at("screen_size").get_value<float>();
                    }
                }
            }
            // The water's look: WaterBody's own keys, onto the terrain's stock look.
            if (node.contains("water") && node.at("water").is_mapping()) {
                water::parse_water_settings(node.at("water"), terrain->water_look);
            }
            if (node.contains("max_chunk_jobs_per_frame")) {
                terrain->max_chunk_jobs_per_frame = node.at("max_chunk_jobs_per_frame").get_value<int>();
            }
            if (node.contains("max_chunks_in_flight")) {
                terrain->max_chunks_in_flight = node.at("max_chunks_in_flight").get_value<int>();
            }
            if (node.contains("upload_budget_ms")) {
                terrain->upload_budget_ms = node.at("upload_budget_ms").get_value<float>();
            }
            if (node.contains("collision_radius")) {
                terrain->collision_radius = node.at("collision_radius").get_value<int>();
            }
            if (node.contains("shadow_distance")) {
                terrain->shadow_distance = node.at("shadow_distance").get_value<float>();
            }
            if (node.contains("dormant_chunks")) {
                terrain->dormant_chunks = node.at("dormant_chunks").get_value<int>();
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
            // The store hands back the same live tiles on every re-parse (unsaved edits included),
            // and an empty grid -- baked from the generator keys on first use -- when there is no file.
            terrain->tiles_file = node.contains("tiles") ? node.at("tiles").get_value<std::string>()
                                                         : "terrain/" + obj.name() + ".tiles";
            // Strictly beside the scene -- never through the asset search, whose by-name fallback
            // would hand two scenes' same-named terrains one shared file.
            {
                const std::string path = std::filesystem::path(terrain->tiles_file).is_absolute()
                                             ? terrain->tiles_file
                                             : (std::filesystem::path(ctx.base_dir()) / terrain->tiles_file).lexically_normal().string();
                terrain->tiles = world::TileMapStore::instance().acquire(path);
            }

            read(node, "collision", terrain->collision);
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
    // One entry point under all three names. `Terrain` is the unified spelling; `TileTerrain`
    // and `HeightmapTerrain` are what existing scenes say, and each still loads exactly as it
    // did -- the mode comes from `mode:` when a scene gives one and from the keys otherwise
    // (world::infer_terrain_mode, terrain_yaml.h). A scene can therefore switch a terrain's mode
    // without renaming its component, and nothing has to be migrated to keep working.
    auto parse_terrain =
        [&assets, parse_tile_terrain](const fkyaml::node& node, SceneObject& obj,
                                      const SceneLoader::ParseContext& ctx) {
            if (world::infer_terrain_mode(node) == world::TerrainMode::Heightmap) {
                world::parse_heightmap_terrain(node, obj, ctx, assets);
            } else {
                parse_tile_terrain(node, obj, ctx);
            }
        };
    SceneLoader::register_component_parser("Terrain", parse_terrain);
    SceneLoader::register_component_parser("TileTerrain", parse_terrain);
    SceneLoader::register_component_parser("HeightmapTerrain", parse_terrain);

    // The map a terrain is generated from -- its own component, applied on demand.
    mapgen::register_map_generator_component();

    // A lake, ocean or river -- see toyengine/water/water_body.h. Baked and published (mesh +
    // material params on the sibling MeshRenderer) by toy::water::WaterSystem, which Engine
    // installs. `mesh_path` loads CPU-side only (a SkinnedMeshSource, since the bake needs the
    // vertices), so the sibling MeshRenderer must not name the same mesh itself.
    SceneLoader::register_component_parser("WaterBody",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* w = obj.add_component<water::WaterBody>();
            if (node.contains("mode")) {
                std::string mode = node.at("mode").get_value<std::string>();
                for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (mode == "flowing" || mode == "river") w->mode = water::WaterMode::Flowing;
                else if (mode == "planar" || mode == "static" || mode == "lake") w->mode = water::WaterMode::Planar;
                else throw std::runtime_error("[WaterBody] unknown mode '" + mode + "' (planar | flowing)");
            }
            read(node, "mesh_path", w->mesh_path);
            if (node.contains("size")) {
                const auto& sz = node.at("size");
                if (sz.contains("x")) w->size.x = sz.at("x").get_value<float>();
                if (sz.contains("y")) w->size.y = sz.at("y").get_value<float>();
            }
            read(node, "resolution", w->resolution);
            read(node, "tile_size", w->tile_size);

            water::parse_water_settings(node, *w);

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
            if (node.contains("preserve_volume")) smr->set_preserve_volume(node.at("preserve_volume").get_value<bool>());
        });
}

} // namespace scene
} // namespace toy
