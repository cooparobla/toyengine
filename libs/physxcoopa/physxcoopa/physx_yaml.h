/**
 * @file physx_yaml.h
 * @brief register_physics_components() -- wires physxcoopa's components into SceneLoader.
 *        install_physics_system() (system/physics_system.h) is the other half of setup.
 */

#ifndef PHYSXCOOPA_PHYSX_YAML_H
#define PHYSXCOOPA_PHYSX_YAML_H

#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/convex_collider.h>
#include <physxcoopa/components/mesh_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/vehicle.h>
#include <physxcoopa/components/hinge_joint.h>
#include <physxcoopa/components/ball_joint.h>
#include <physxcoopa/components/cone_twist_joint.h>
#include <physxcoopa/components/cloth.h>
#include <physxcoopa/components/damage_dealer.h>
#include <physxcoopa/damage/damage_material.h>
#include <physxcoopa/damage/damage_types.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/loaders/triangle_mesh_loader.h>
#include <physxcoopa/loaders/physics_material_loader.h>
#include <physxcoopa/loaders/damage_material_loader.h>
#include <physxcoopa/util/physics_settings.h>
#include <physxcoopa/nav_yaml.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/asset/asset_manager.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace coopa {
namespace physx {

namespace detail {

inline glm::vec3 read_vec3_(const fkyaml::node& node, const std::string& key, const glm::vec3& fallback) {
    if (!node.contains(key)) return fallback;
    const auto& sub = node.at(key);
    glm::vec3 v = fallback;
    if (sub.contains("x")) v.x = sub.at("x").get_value<float>();
    if (sub.contains("y")) v.y = sub.at("y").get_value<float>();
    if (sub.contains("z")) v.z = sub.at("z").get_value<float>();
    return v;
}

/**
 * @brief Process-wide layer-name registry, populated by register_physics_components()'s
 *        settings-aware overload from PhysicsSettings::layer_names -- lets scene YAML write
 *        `layer: "Ground"` in addition to a bare integer. Mirrors SceneLoader's own
 *        function-local-static registry pattern; scene loading is single-threaded in this
 *        codebase (see SceneLoader::load()'s own doc), so no locking is needed here either.
 */
inline std::vector<std::string>& layer_registry_() {
    static std::vector<std::string> names;
    return names;
}

/** @brief Resolves `node[key]` as either a layer name (looked up in layer_registry_(), 0 if
 *         unknown) or a bare integer, defaulting to `fallback` if the key is absent. */
inline uint32_t read_layer_(const fkyaml::node& node, const std::string& key, uint32_t fallback) {
    if (!node.contains(key)) return fallback;
    const auto& v = node.at(key);
    if (v.is_string()) {
        std::string name = v.get_value<std::string>();
        const auto& names = layer_registry_();
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) return static_cast<uint32_t>(i);
        }
        return fallback;
    }
    return v.get_value<uint32_t>();
}

/** @brief Parses an inline `material: { dynamic_friction: ..., ... }` mapping directly into a
 *         PhysicsMaterial -- the non-asset half of Collider::material(); shares its field
 *         parsing with loaders::PhysicsMaterialLoader but reads from an already-open node
 *         rather than a standalone file. */
inline dynamics::PhysicsMaterial parse_inline_material_(const fkyaml::node& node) {
    dynamics::PhysicsMaterial mat;
    if (node.contains("dynamic_friction")) mat.dynamic_friction = node.at("dynamic_friction").get_value<float>();
    if (node.contains("static_friction")) mat.static_friction = node.at("static_friction").get_value<float>();
    if (node.contains("restitution")) mat.restitution = node.at("restitution").get_value<float>();
    auto combine_of = [&](const char* key, dynamics::CombineMode fallback) {
        if (!node.contains(key)) return fallback;
        std::string v = node.at(key).get_value<std::string>();
        if (v == "Average") return dynamics::CombineMode::Average;
        if (v == "Minimum") return dynamics::CombineMode::Minimum;
        if (v == "Maximum") return dynamics::CombineMode::Maximum;
        if (v == "Multiply") return dynamics::CombineMode::Multiply;
        return fallback;
    };
    mat.friction_combine = combine_of("friction_combine", mat.friction_combine);
    mat.restitution_combine = combine_of("restitution_combine", mat.restitution_combine);
    return mat;
}

/**
 * @brief Applies a collider's `material:` key, if present -- a string loads a shared
 *        `physics_materials/<name>.yaml` asset (set_material_asset()); a mapping parses an
 *        inline, not-asset-tracked PhysicsMaterial (set_material()). Absent leaves the
 *        collider on PhysicsMaterial::default_material(), matching a fresh Collider's default.
 */
inline void apply_material_(const fkyaml::node& node, components::Collider& collider,
                             coopa::asset::AssetManager& assets, const coopa::scene::SceneLoader::ParseContext& ctx) {
    if (!node.contains("material")) return;
    const auto& m = node.at("material");
    if (m.is_string()) {
        std::string virtual_path = "physics_materials/" + m.get_value<std::string>() + ".yaml";
        collider.set_material_asset(assets.load<dynamics::PhysicsMaterial>(virtual_path, ctx.base_dir()));
    } else if (m.is_mapping()) {
        collider.set_material(std::make_shared<dynamics::PhysicsMaterial>(parse_inline_material_(m)));
    }
}

/** @brief Common fields every Collider subclass's parser applies, beyond its own shape params. */
inline void apply_common_collider_fields_(const fkyaml::node& node, components::Collider& collider,
                                           coopa::asset::AssetManager& assets,
                                           const coopa::scene::SceneLoader::ParseContext& ctx) {
    collider.set_center(read_vec3_(node, "center", glm::vec3(0.0f)));
    if (node.contains("is_trigger")) collider.set_is_trigger(node.at("is_trigger").get_value<bool>());
    if (node.contains("enabled")) collider.set_enabled(node.at("enabled").get_value<bool>());
    collider.set_layer(read_layer_(node, "layer", collider.layer()));
    apply_material_(node, collider, assets, ctx);
}

/** @brief Parses `freeze_position`/`freeze_rotation: {x,y,z}` (each axis a bool) into
 *         dynamics::BodyConstraints bits, OR'd onto whatever `constraints` already holds. */
inline uint32_t read_freeze_flags_(const fkyaml::node& node, const std::string& key, uint32_t constraints,
                                    uint32_t x_bit, uint32_t y_bit, uint32_t z_bit) {
    if (!node.contains(key)) return constraints;
    const auto& sub = node.at(key);
    if (sub.contains("x") && sub.at("x").get_value<bool>()) constraints |= x_bit;
    if (sub.contains("y") && sub.at("y").get_value<bool>()) constraints |= y_bit;
    if (sub.contains("z") && sub.at("z").get_value<bool>()) constraints |= z_bit;
    return constraints;
}

/** @brief `node[key]` as a float if present, else `fallback`. */
inline float read_float_(const fkyaml::node& node, const char* key, float fallback) {
    return node.contains(key) ? node.at(key).get_value<float>() : fallback;
}

/** @brief A DifferentialType name (`open` / `limited_slip` / `locked`), else `fallback`. */
inline vehicle::DifferentialType read_differential_(const fkyaml::node& node, const char* key,
                                                     vehicle::DifferentialType fallback) {
    if (!node.contains(key)) return fallback;
    const std::string v = node.at(key).get_value<std::string>();
    if (v == "open") return vehicle::DifferentialType::Open;
    if (v == "limited_slip") return vehicle::DifferentialType::LimitedSlip;
    if (v == "locked") return vehicle::DifferentialType::Locked;
    return fallback;
}

/** @brief Per-wheel overrides on top of whatever the preset put in this slot. */
inline void read_wheel_overrides_(const fkyaml::node& node, vehicle::WheelConfig& w) {
    w.anchor = read_vec3_(node, "anchor", w.anchor);
    w.radius = read_float_(node, "radius", w.radius);
    w.width = read_float_(node, "width", w.width);
    w.mass = read_float_(node, "mass", w.mass);
    w.rest_length = read_float_(node, "rest_length", w.rest_length);
    w.travel = read_float_(node, "travel", w.travel);
    w.stiffness = read_float_(node, "stiffness", w.stiffness);
    w.natural_frequency = read_float_(node, "natural_frequency", w.natural_frequency);
    w.damping_ratio = read_float_(node, "damping_ratio", w.damping_ratio);
    w.rebound_ratio = read_float_(node, "rebound_ratio", w.rebound_ratio);
    w.drive_share = read_float_(node, "drive_share", w.drive_share);
    w.brake_share = read_float_(node, "brake_share", w.brake_share);
    w.handbrake_share = read_float_(node, "handbrake_share", w.handbrake_share);
    if (node.contains("steered")) w.steered = node.at("steered").get_value<bool>();
    if (node.contains("anti_roll_group")) w.anti_roll_group = node.at("anti_roll_group").get_value<int>();
    if (node.contains("side")) {
        w.side = node.at("side").get_value<std::string>() == "right" ? vehicle::WheelSide::Right
                                                                     : vehicle::WheelSide::Left;
    }
}

/** @brief The keys every joint component shares beyond connected_object/anchor. */
inline void read_joint_common_(const fkyaml::node& node, components::JointComponent& jc) {
    if (node.contains("enable_collision")) jc.enable_collision = node.at("enable_collision").get_value<bool>();
}

/** @brief Parses a DamageType name (case-sensitive, matching the enum's spelling). Unknown or
 *         absent values fall back to `fallback` rather than throwing, same policy as every
 *         other enum-ish key in this file. */
inline damage::DamageType parse_damage_type_(const fkyaml::node& node, const std::string& key,
                                             damage::DamageType fallback) {
    if (!node.contains(key)) return fallback;
    std::string v = node.at(key).get_value<std::string>();
    if (v == "Impact") return damage::DamageType::Impact;
    if (v == "Slash") return damage::DamageType::Slash;
    if (v == "Pierce") return damage::DamageType::Pierce;
    if (v == "Explosive") return damage::DamageType::Explosive;
    if (v == "Fire") return damage::DamageType::Fire;
    return fallback;
}

} // namespace detail

/**
 * @brief Registers every physxcoopa component's YAML parser with SceneLoader.
 *
 * Each name is registered once -- SceneLoader::normalize_tag_() strips a leading '!' before
 * every lookup, so a single "BoxCollider" registration matches both a `!BoxCollider` YAML
 * tag and a `type: BoxCollider` key (see the plan's constraint 5).
 *
 * @param assets   AssetManager used to resolve MeshCollider's/ConvexCollider's mesh reference,
 *                 colliders' named `material:` references and DamageDealer-adjacent
 *                 `damage_materials/<name>.yaml` lookups (loaders/damage_material_loader.h).
 *                 Must outlive every subsequent SceneLoader::load() call, matching
 *                 gfxcoopa::register_render_components()'s convention.
 * @param settings Project-wide physics settings (see util/physics_settings.h) -- specifically,
 *                 seeds the layer-name registry so `layer: "Ground"` resolves. Defaults to an
 *                 empty PhysicsSettings (no named layers; `layer:` must be a bare integer).
 */
inline void register_physics_components(coopa::asset::AssetManager& assets,
                                         const util::PhysicsSettings& settings = {}) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    detail::layer_registry_() = settings.layer_names;

    assets.register_loader<geometry::TriangleMesh>(std::make_unique<loaders::TriangleMeshLoader>());
    assets.register_loader<dynamics::PhysicsMaterial>(std::make_unique<loaders::PhysicsMaterialLoader>());
    assets.register_loader<damage::DamageMaterial>(std::make_unique<loaders::DamageMaterialLoader>());

    SceneLoader::register_component_parser("BoxCollider",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<components::BoxCollider>();
            c->set_size(detail::read_vec3_(node, "size", glm::vec3(1.0f)));
            detail::apply_common_collider_fields_(node, *c, assets, ctx);
        });

    SceneLoader::register_component_parser("SphereCollider",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<components::SphereCollider>();
            if (node.contains("radius")) c->set_radius(node.at("radius").get_value<float>());
            detail::apply_common_collider_fields_(node, *c, assets, ctx);
        });

    SceneLoader::register_component_parser("CapsuleCollider",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<components::CapsuleCollider>();
            if (node.contains("radius")) c->set_radius(node.at("radius").get_value<float>());
            if (node.contains("height")) c->set_height(node.at("height").get_value<float>());
            if (node.contains("direction")) c->set_direction(node.at("direction").get_value<int>());
            detail::apply_common_collider_fields_(node, *c, assets, ctx);
        });

    SceneLoader::register_component_parser("MeshCollider",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<components::MeshCollider>();
            if (node.contains("convex")) c->set_convex(node.at("convex").get_value<bool>());
            if (node.contains("mesh_path")) {
                std::string key = node.at("mesh_path").get_value<std::string>();
                std::string virtual_path = "meshes/" + key + ".yaml";
                c->set_mesh(assets.load<geometry::TriangleMesh>(virtual_path, ctx.base_dir()));
            }
            detail::apply_common_collider_fields_(node, *c, assets, ctx);
        });

    SceneLoader::register_component_parser("ConvexCollider",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<components::ConvexCollider>();
            if (node.contains("hull_mesh")) {
                std::string key = node.at("hull_mesh").get_value<std::string>();
                std::string virtual_path = "meshes/" + key + ".yaml";
                c->set_hull_mesh(assets.load<geometry::TriangleMesh>(virtual_path, ctx.base_dir()));
            }
            detail::apply_common_collider_fields_(node, *c, assets, ctx);
        });

    // What a collider's hits ARE (components/damage_dealer.h) -- put on a weapon head, a
    // bullet, a falling weight, alongside the Collider that actually touches things.
    SceneLoader::register_component_parser("DamageDealer",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* d = obj.add_component<components::DamageDealer>();
            // The YAML key is "damage_type", not "type": every component node's own "type:"
            // key already names WHICH COMPONENT this is ("type: DamageDealer"), so a field
            // literally called "type" collides with it in the editor's generic schema/default-
            // component machinery (confirmed by component_schema_test.cpp).
            d->type = detail::parse_damage_type_(node, "damage_type", d->type);
            if (node.contains("amount")) d->amount = node.at("amount").get_value<float>();
            if (node.contains("amount_from_impulse")) {
                d->amount_from_impulse = node.at("amount_from_impulse").get_value<float>();
            }
            if (node.contains("edge_axis")) d->edge_axis = detail::read_vec3_(node, "edge_axis", d->edge_axis);
            if (node.contains("radius")) d->radius = node.at("radius").get_value<float>();
            if (node.contains("one_shot")) d->one_shot = node.at("one_shot").get_value<bool>();
        });

    SceneLoader::register_component_parser("Rigidbody",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* rb = obj.add_component<components::RigidbodyComponent>();
            if (node.contains("mass")) rb->mass = node.at("mass").get_value<float>();
            if (node.contains("drag")) rb->drag = node.at("drag").get_value<float>();
            if (node.contains("angular_drag")) rb->angular_drag = node.at("angular_drag").get_value<float>();
            if (node.contains("wind_influence")) rb->wind_influence = node.at("wind_influence").get_value<float>();
            // Where the mass really sits, as an offset from the collider's centre. The single
            // most useful dial a scene has for how a body behaves and, until now, the only one
            // of Rigidbody's that YAML could not reach: a vehicle rolls over when its lateral
            // acceleration passes half-track / centre-of-mass height, so a tall box whose mass
            // sits at its geometric centre tips at well under 1 g.
            if (node.contains("center_of_mass")) {
                rb->center_of_mass_override = detail::read_vec3_(node, "center_of_mass", glm::vec3(0.0f));
            }
            if (node.contains("use_gravity")) rb->use_gravity = node.at("use_gravity").get_value<bool>();
            if (node.contains("is_kinematic")) rb->is_kinematic = node.at("is_kinematic").get_value<bool>();
            if (node.contains("interpolation")) {
                std::string v = node.at("interpolation").get_value<std::string>();
                rb->interpolation = (v == "None") ? dynamics::Interpolation::None : dynamics::Interpolation::Interpolate;
            }
            rb->constraints = detail::read_freeze_flags_(node, "freeze_position", rb->constraints,
                dynamics::kFreezePosX, dynamics::kFreezePosY, dynamics::kFreezePosZ);
            rb->constraints = detail::read_freeze_flags_(node, "freeze_rotation", rb->constraints,
                dynamics::kFreezeRotX, dynamics::kFreezeRotY, dynamics::kFreezeRotZ);
            rb->initial_velocity = detail::read_vec3_(node, "velocity", glm::vec3(0.0f));
            rb->initial_angular_velocity = detail::read_vec3_(node, "angular_velocity", glm::vec3(0.0f));
        });

    SceneLoader::register_component_parser("HingeJoint",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* jc = obj.add_component<components::HingeJointComponent>();
            if (node.contains("connected_object")) jc->connected_object = node.at("connected_object").get_value<std::string>();
            jc->anchor = detail::read_vec3_(node, "anchor", glm::vec3(0.0f));
            jc->axis = detail::read_vec3_(node, "axis", glm::vec3(0.0f, 0.0f, 1.0f));
            if (node.contains("limits")) {
                const auto& limits = node.at("limits");
                jc->use_limits = true;
                if (limits.contains("min")) jc->min_angle_deg = limits.at("min").get_value<float>();
                if (limits.contains("max")) jc->max_angle_deg = limits.at("max").get_value<float>();
            }
            // The flat spelling the editor writes (a schema field per key).
            if (node.contains("use_limits")) jc->use_limits = node.at("use_limits").get_value<bool>();
            if (node.contains("min_angle")) jc->min_angle_deg = node.at("min_angle").get_value<float>();
            if (node.contains("max_angle")) jc->max_angle_deg = node.at("max_angle").get_value<float>();
            detail::read_joint_common_(node, *jc);
        });

    SceneLoader::register_component_parser("BallJoint",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* jc = obj.add_component<components::BallJointComponent>();
            if (node.contains("connected_object")) jc->connected_object = node.at("connected_object").get_value<std::string>();
            jc->anchor = detail::read_vec3_(node, "anchor", glm::vec3(0.0f));
            detail::read_joint_common_(node, *jc);
        });

    // swing_limit is the cone half-angle in degrees (negative = free); `twist: {min, max}` (or flat
    // twist_min / twist_max) the twist range in degrees (absent = +-30; min > max = free).
    SceneLoader::register_component_parser("ConeTwistJoint",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* jc = obj.add_component<components::ConeTwistJointComponent>();
            if (node.contains("connected_object")) jc->connected_object = node.at("connected_object").get_value<std::string>();
            jc->anchor = detail::read_vec3_(node, "anchor", glm::vec3(0.0f));
            jc->axis = detail::read_vec3_(node, "axis", glm::vec3(0.0f, 0.0f, 1.0f));
            if (node.contains("swing_limit")) jc->swing_limit_deg = node.at("swing_limit").get_value<float>();
            if (node.contains("twist")) {
                const auto& twist = node.at("twist");
                if (twist.contains("min")) jc->twist_min_deg = twist.at("min").get_value<float>();
                if (twist.contains("max")) jc->twist_max_deg = twist.at("max").get_value<float>();
            }
            if (node.contains("twist_min")) jc->twist_min_deg = node.at("twist_min").get_value<float>();
            if (node.contains("twist_max")) jc->twist_max_deg = node.at("twist_max").get_value<float>();
            detail::read_joint_common_(node, *jc);
        });

    // Every ClothParams field is authorable, flat, under the component's own mapping rather than
    // a nested `params:` block -- the component IS the cloth's parameter set, and a scene author
    // reading `friction: 0.5` next to `thickness: 0.03` should not have to know which of the two
    // the C++ side happens to keep in a sub-struct. `anchors` is the only sequence; each entry's
    // `object` is resolved later by PhysicsSystem::regather_cloths_(), never here (the named
    // sibling may not exist yet while this parser runs -- see ClothComponent's doc).
    SceneLoader::register_component_parser("Cloth",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<components::ClothComponent>();

            if (node.contains("resolution")) {
                const auto& r = node.at("resolution");
                if (r.contains("x")) cc->columns = r.at("x").get_value<uint32_t>();
                if (r.contains("y")) cc->rows = r.at("y").get_value<uint32_t>();
            }
            if (node.contains("size")) {
                const auto& sz = node.at("size");
                if (sz.contains("x")) cc->width = sz.at("x").get_value<float>();
                if (sz.contains("y")) cc->height = sz.at("y").get_value<float>();
            }
            if (node.contains("mass")) cc->mass = node.at("mass").get_value<float>();
            if (node.contains("shear")) cc->shear = node.at("shear").get_value<bool>();

            cloth::ClothParams& p = cc->params;
            if (node.contains("stretch_compliance")) p.stretch_compliance = node.at("stretch_compliance").get_value<float>();
            if (node.contains("bend_compliance")) p.bend_compliance = node.at("bend_compliance").get_value<float>();
            if (node.contains("damping")) p.damping = node.at("damping").get_value<float>();
            if (node.contains("thickness")) p.thickness = node.at("thickness").get_value<float>();
            if (node.contains("friction")) p.friction = node.at("friction").get_value<float>();
            if (node.contains("gravity_scale")) p.gravity_scale = node.at("gravity_scale").get_value<float>();
            if (node.contains("max_velocity")) p.max_velocity = node.at("max_velocity").get_value<float>();
            p.external_acceleration = detail::read_vec3_(node, "external_acceleration", p.external_acceleration);
            p.wind = detail::read_vec3_(node, "wind", p.wind);
            if (node.contains("wind_turbulence")) p.wind_turbulence = node.at("wind_turbulence").get_value<float>();
            if (node.contains("weather_wind")) cc->weather_wind = node.at("weather_wind").get_value<float>();
            if (node.contains("air_drag")) p.air_drag = node.at("air_drag").get_value<float>();
            if (node.contains("air_lift")) p.air_lift = node.at("air_lift").get_value<float>();
            if (node.contains("self_collision")) p.self_collision = node.at("self_collision").get_value<bool>();
            if (node.contains("self_distance")) p.self_distance = node.at("self_distance").get_value<float>();
            if (node.contains("tether_scale")) p.tether_scale = node.at("tether_scale").get_value<float>();
            if (node.contains("substeps")) p.substeps = node.at("substeps").get_value<uint32_t>();
            if (node.contains("iterations")) p.iterations = node.at("iterations").get_value<uint32_t>();
            if (node.contains("sleep_threshold")) p.sleep_threshold = node.at("sleep_threshold").get_value<float>();
            if (node.contains("sleep_time")) p.sleep_time = node.at("sleep_time").get_value<float>();
            p.layer = detail::read_layer_(node, "layer", p.layer);

            if (node.contains("anchors")) {
                for (const auto& a : node.at("anchors")) {
                    components::ClothAnchorSpec spec;
                    if (a.contains("object")) spec.object = a.at("object").get_value<std::string>();
                    spec.point = detail::read_vec3_(a, "point", glm::vec3(0.0f));
                    if (a.contains("radius")) spec.radius = a.at("radius").get_value<float>();
                    cc->anchors.push_back(spec);
                }
            }
        });

    // A driveable vehicle (vehicle/vehicle_sim.h) on this object's Rigidbody chassis -- inert
    // until install_vehicle_system(). `preset:` picks a tuning and every other key overrides one
    // of its values, so a scene writes the handful it cares about and nothing else.
    SceneLoader::register_component_parser("Vehicle",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* vc = obj.add_component<components::VehicleComponent>();

            if (node.contains("preset")) vc->preset = node.at("preset").get_value<std::string>();
            vc->config = vehicle::preset(vc->preset);

            vehicle::VehicleConfig& c = vc->config;
            if (node.contains("steer_mode")) {
                c.steer_mode = node.at("steer_mode").get_value<std::string>() == "differential"
                                   ? vehicle::SteerMode::Differential
                                   : vehicle::SteerMode::Ackermann;
            }
            c.max_steer_deg = detail::read_float_(node, "max_steer_deg", c.max_steer_deg);
            c.min_steer_deg = detail::read_float_(node, "min_steer_deg", c.min_steer_deg);
            c.steer_falloff_speed = detail::read_float_(node, "steer_falloff_speed", c.steer_falloff_speed);
            c.steer_rate_deg = detail::read_float_(node, "steer_rate_deg", c.steer_rate_deg);
            c.steer_return_rate_deg = detail::read_float_(node, "steer_return_rate_deg", c.steer_return_rate_deg);
            c.brake_torque = detail::read_float_(node, "brake_torque", c.brake_torque);
            c.handbrake_torque = detail::read_float_(node, "handbrake_torque", c.handbrake_torque);
            c.anti_roll = detail::read_float_(node, "anti_roll", c.anti_roll);
            c.drag = detail::read_float_(node, "drag", c.drag);
            c.downforce = detail::read_float_(node, "downforce", c.downforce);
            c.air_righting = detail::read_float_(node, "air_righting", c.air_righting);
            c.air_damping = detail::read_float_(node, "air_damping", c.air_damping);
            c.nominal_wheel_load = detail::read_float_(node, "nominal_wheel_load", c.nominal_wheel_load);
            c.recover_time = detail::read_float_(node, "recover_time", c.recover_time);
            c.recover_lift = detail::read_float_(node, "recover_lift", c.recover_lift);
            c.auto_recover_time = detail::read_float_(node, "auto_recover_time", c.auto_recover_time);

            if (node.contains("tire")) {
                const auto& t = node.at("tire");
                c.tire.grip = detail::read_float_(t, "grip", c.tire.grip);
                c.tire.lateral_grip = detail::read_float_(t, "lateral_grip", c.tire.lateral_grip);
                c.tire.peak_slip_ratio = detail::read_float_(t, "peak_slip_ratio", c.tire.peak_slip_ratio);
                c.tire.peak_slip_angle = detail::read_float_(t, "peak_slip_angle", c.tire.peak_slip_angle);
                c.tire.plateau = detail::read_float_(t, "plateau", c.tire.plateau);
                c.tire.falloff = detail::read_float_(t, "falloff", c.tire.falloff);
                c.tire.rolling_resistance = detail::read_float_(t, "rolling_resistance", c.tire.rolling_resistance);
                c.tire.load_sensitivity = detail::read_float_(t, "load_sensitivity", c.tire.load_sensitivity);
            }
            if (node.contains("engine")) {
                const auto& e = node.at("engine");
                c.engine.idle_rpm = detail::read_float_(e, "idle_rpm", c.engine.idle_rpm);
                c.engine.max_rpm = detail::read_float_(e, "max_rpm", c.engine.max_rpm);
                c.engine.peak_torque = detail::read_float_(e, "peak_torque", c.engine.peak_torque);
                c.engine.peak_rpm = detail::read_float_(e, "peak_rpm", c.engine.peak_rpm);
                c.engine.torque_at_idle = detail::read_float_(e, "torque_at_idle", c.engine.torque_at_idle);
                c.engine.torque_at_max = detail::read_float_(e, "torque_at_max", c.engine.torque_at_max);
                c.engine.braking = detail::read_float_(e, "braking", c.engine.braking);
                c.engine.response = detail::read_float_(e, "response", c.engine.response);
            }
            if (node.contains("gearbox")) {
                const auto& g = node.at("gearbox");
                if (g.contains("forward_ratios")) {
                    c.gearbox.forward_ratios.clear();
                    for (const auto& r : g.at("forward_ratios")) {
                        c.gearbox.forward_ratios.push_back(r.get_value<float>());
                    }
                }
                c.gearbox.reverse_ratio = detail::read_float_(g, "reverse_ratio", c.gearbox.reverse_ratio);
                c.gearbox.final_drive = detail::read_float_(g, "final_drive", c.gearbox.final_drive);
                c.gearbox.efficiency = detail::read_float_(g, "efficiency", c.gearbox.efficiency);
                if (g.contains("automatic")) c.gearbox.automatic = g.at("automatic").get_value<bool>();
                c.gearbox.shift_up_rpm = detail::read_float_(g, "shift_up_rpm", c.gearbox.shift_up_rpm);
                c.gearbox.shift_down_rpm = detail::read_float_(g, "shift_down_rpm", c.gearbox.shift_down_rpm);
                c.gearbox.shift_time = detail::read_float_(g, "shift_time", c.gearbox.shift_time);
            }
            if (node.contains("drive")) {
                const auto& d = node.at("drive");
                c.drive.differential = detail::read_differential_(d, "differential", c.drive.differential);
                c.drive.limited_slip = detail::read_float_(d, "limited_slip", c.drive.limited_slip);
                c.drive.coupling = detail::read_float_(d, "coupling", c.drive.coupling);
            }

            // `wheels:` is positional against the preset's own wheel list: entry i overrides
            // wheel i and names the child object drawn there. More entries than the preset has
            // wheels APPEND new ones (a bespoke layout), which is why the resize is unconditional.
            if (node.contains("wheels")) {
                const auto& list = node.at("wheels");
                std::size_t i = 0;
                for (const auto& w : list) {
                    if (i >= c.wheels.size()) {
                        c.wheels.push_back(c.wheels.empty() ? vehicle::WheelConfig{} : c.wheels.back());
                    }
                    detail::read_wheel_overrides_(w, c.wheels[i]);
                    vc->wheel_objects.resize(i + 1);
                    if (w.contains("object")) vc->wheel_objects[i] = w.at("object").get_value<std::string>();
                    ++i;
                }
            }

            if (node.contains("player_input")) vc->player_input = node.at("player_input").get_value<bool>();
            if (node.contains("active")) vc->active = node.at("active").get_value<bool>();
        });

    // NavAgent / NavModifier / NavVolume / NavLink -- inert until install_nav_system().
    register_nav_components();
}

} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_PHYSX_YAML_H
