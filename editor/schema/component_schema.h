/**
 * @file component_schema.h
 * @brief Describes each component type's YAML keys for the inspector and "Add Component".
 *
 * There is no reflection in the engine: every component parser reads its keys by hand
 * (see gfxcoopa's components/register.h, toyengine/scene/register.h, physxcoopa's
 * physx_yaml.h). A schema mirrors those keys -- label, widget kind, range, default -- so
 * the inspector can show friendly widgets and "Add Component" can write a sensible
 * starting block. Schemas only DESCRIBE: the inspector edits the component's YAML node,
 * and keys a schema doesn't list (or whole component types without a schema) are still
 * shown and edited generically from their values, and always saved verbatim.
 */

#ifndef TOYEDITOR_SCHEMA_COMPONENT_SCHEMA_H
#define TOYEDITOR_SCHEMA_COMPONENT_SCHEMA_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <vector>

namespace toy::editor {

enum class FieldKind {
    Bool, Int, Float, Vec3, Vec4, Color, Enum, String,
    AssetRef,   ///< A path into the project's assets (dir + extension); a dropdown of files.
    Material,   ///< A `material:` value: inline block, asset reference, or asset + overrides.
};

struct FieldDesc {
    std::string key;
    FieldKind   kind = FieldKind::Float;
    std::string label;                 ///< Empty: derived from the key.
    float       speed = 0.01f;         ///< Drag speed for numbers.
    float       min = -1e30f, max = 1e30f;
    glm::vec4   def{0.0f};             ///< Default (x for scalars, xyz vectors/colours).
    std::vector<std::string> options;  ///< Enum values.
    std::string asset_dir;             ///< AssetRef: "meshes", "materials", "textures", ...
    std::string asset_ext;             ///< AssetRef: ".yaml", ".png", ...
    bool        strip_ext = false;     ///< AssetRef: store "name" instead of "dir/name.ext" (meshes).
    bool        strip_dir = false;     ///< AssetRef: store without the "dir/" prefix (physics materials).
    bool        in_default = false;    ///< Written into a freshly added component.
    std::string ref_prefix;            ///< AssetRef: prepended to the stored value ("assets/" for config paths).
    bool        as_list = false;       ///< Vec3/Color stored as [a, b, c] (config.yaml) not a map.
    bool        startup_only = false;  ///< Render settings: needs a renderer restart to apply.
    std::string default_string;
    std::string tooltip;

    std::string display() const {
        if (!label.empty()) return label;
        std::string s = key;
        for (char& c : s) if (c == '_') c = ' ';
        if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        return s;
    }
};

struct ComponentSchema {
    std::string type;
    std::string category;              ///< "Rendering", "Physics", ... for the Add menu.
    std::vector<FieldDesc> fields;
    bool unique = true;                ///< At most one per object (Transform, Rigidbody...).
    bool removable = true;
};

// --- field builders (keep the tables below readable) ---
inline FieldDesc f_bool(std::string k, bool def, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Bool; f.def.x = def ? 1.0f : 0.0f; f.in_default = in_default; return f;
}
inline FieldDesc f_int(std::string k, int def, int lo = -1000000, int hi = 1000000, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Int; f.def.x = static_cast<float>(def);
    f.min = static_cast<float>(lo); f.max = static_cast<float>(hi); f.speed = 0.2f; f.in_default = in_default; return f;
}
inline FieldDesc f_float(std::string k, float def, float speed = 0.01f, float lo = -1e30f, float hi = 1e30f, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Float; f.def.x = def; f.speed = speed; f.min = lo; f.max = hi;
    f.in_default = in_default; return f;
}
inline FieldDesc f_vec3(std::string k, glm::vec3 def, float speed = 0.01f, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Vec3; f.def = glm::vec4(def, 0.0f); f.speed = speed; f.in_default = in_default; return f;
}
inline FieldDesc f_color(std::string k, glm::vec3 def, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Color; f.def = glm::vec4(def, 1.0f); f.in_default = in_default; return f;
}
inline FieldDesc f_enum(std::string k, std::vector<std::string> opts, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Enum; f.options = std::move(opts);
    f.default_string = f.options.empty() ? "" : f.options.front(); f.in_default = in_default; return f;
}
inline FieldDesc f_string(std::string k, std::string def = "", bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::String; f.default_string = std::move(def); f.in_default = in_default; return f;
}
inline FieldDesc f_asset(std::string k, std::string dir, std::string ext, bool strip_ext = false, bool strip_dir = false,
                         std::string def = "", bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::AssetRef; f.asset_dir = std::move(dir); f.asset_ext = std::move(ext);
    f.strip_ext = strip_ext; f.strip_dir = strip_dir; f.default_string = std::move(def); f.in_default = in_default; return f;
}
inline FieldDesc f_material() { FieldDesc f; f.key = "material"; f.kind = FieldKind::Material; f.in_default = true; return f; }
inline FieldDesc with_label(FieldDesc f, std::string l) { f.label = std::move(l); return f; }
inline FieldDesc listed(FieldDesc f) { f.as_list = true; return f; }
inline FieldDesc root_relative(FieldDesc f) { f.ref_prefix = "assets/"; return f; }
inline FieldDesc startup(FieldDesc f) { f.startup_only = true; return f; }

/** @brief The keys of a PBRMaterial block (inline, or a materials/*.yaml asset). */
inline const std::vector<FieldDesc>& material_fields() {
    static const std::vector<FieldDesc> fields = {
        f_color("albedo", glm::vec3(0.8f), true),
        f_float("metallic", 0.0f, 0.005f, 0.0f, 1.0f, true),
        f_float("roughness", 0.5f, 0.005f, 0.0f, 1.0f, true),
        f_float("ao", 1.0f, 0.005f, 0.0f, 1.0f),
        f_color("emissive", glm::vec3(0.0f)),
        f_float("emissive_strength", 0.0f, 0.02f, 0.0f, 1000.0f),
        f_enum("alpha_mode", {"OPAQUE", "MASK", "BLEND"}),
        f_float("alpha", 1.0f, 0.005f, 0.0f, 1.0f),
        f_float("alpha_cutoff", 0.5f, 0.005f, 0.0f, 1.0f),
        f_bool("cull_backfaces", true),
        f_bool("refraction", false),
        f_float("ior", 1.33f, 0.005f, 1.0f, 3.0f),
        f_float("refraction_thickness", 0.1f, 0.005f, 0.0f, 10.0f),
        f_color("refraction_tint", glm::vec3(1.0f)),
        with_label(f_asset("texture_albedo", "textures", ".png"), "Albedo map"),
        with_label(f_asset("texture_normal", "textures", ".png"), "Normal map"),
        with_label(f_asset("texture_metallic_roughness", "textures", ".png"), "Metal/rough map"),
        with_label(f_asset("texture_alpha_mask", "textures", ".png"), "Alpha mask"),
        f_enum("shader", {"", "foliage", "terrain", "water"}),
        [] { FieldDesc f; f.key = "shader_params"; f.kind = FieldKind::Vec4; f.speed = 0.01f; return f; }(),
    };
    return fields;
}

/** @brief Every built-in component schema, keyed by type name. */
inline const std::map<std::string, ComponentSchema>& schemas() {
    static const std::map<std::string, ComponentSchema> table = [] {
        std::map<std::string, ComponentSchema> t;
        auto add = [&](ComponentSchema s) { t[s.type] = std::move(s); };
        const auto phys_mat = f_asset("material", "physics_materials", ".yaml", true, true);

        add({"Transform", "Core", {
            f_vec3("position", glm::vec3(0.0f), 0.02f, true),
            f_vec3("rotation", glm::vec3(0.0f), 0.5f, true),
            f_vec3("scale", glm::vec3(1.0f), 0.01f, true),
        }, true, false});
        add({"MeshRenderer", "Rendering", {
            f_asset("mesh_path", "meshes", ".yaml", true, true, "cube", true),
            f_material(),
            f_float("lod_bias", 0.0f, 0.01f),
            f_bool("lods_enabled", true),
            f_bool("affects_reflection_probes", true),
        }});
        add({"Camera", "Rendering", {
            f_bool("main", true, true),
            f_enum("projection", {"Perspective", "Orthographic"}, true),
            f_float("fov", 60.0f, 0.2f, 1.0f, 179.0f, true),
            f_float("orthographic_size", 5.0f, 0.05f, 0.01f, 10000.0f),
            f_float("near_clip_plane", 0.1f, 0.01f, 0.0001f, 1000.0f, true),
            f_float("far_clip_plane", 1000.0f, 1.0f, 0.01f, 100000.0f, true),
            f_float("lens", 50.0f, 0.5f, 1.0f, 1000.0f),
            f_float("aperture", 4.0f, 0.05f, 0.5f, 64.0f),
            f_float("focus_distance", 10.0f, 0.05f, 0.0f, 10000.0f),
            f_string("focus_object"),
        }});
        add({"DirectionalLight", "Lighting", {
            f_vec3("direction", glm::vec3(-0.35f, -0.45f, -0.82f), 0.01f, true),
            f_color("color", glm::vec3(1.0f, 0.97f, 0.9f), true),
            f_float("intensity", 1.0f, 0.01f, 0.0f, 1000.0f, true),
            f_bool("cast_shadows", true, true),
            f_float("shadow_intensity", 0.6f, 0.01f, 0.0f, 1.0f),
        }});
        add({"PointLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_bool("cast_shadows", false, true),
            f_float("attenuation_constant", 4.0f, 0.05f, 0.1f, 64.0f),
        }, false});
        add({"SpotLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_vec3("direction", glm::vec3(0.0f, 0.0f, -1.0f), 0.01f, true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_float("inner_angle", 20.0f, 0.2f, 0.0f, 89.0f, true),
            f_float("outer_angle", 30.0f, 0.2f, 0.0f, 89.0f, true),
            f_bool("cast_shadows", false),
            f_float("attenuation_constant", 4.0f, 0.05f, 0.1f, 64.0f),
        }, false});
        add({"EnvironmentLight", "Lighting", {
            f_color("sky_color", glm::vec3(0.55f, 0.65f, 0.85f), true),
            f_color("ground_color", glm::vec3(0.25f, 0.22f, 0.2f), true),
            f_float("sky_intensity", 1.0f, 0.01f, 0.0f, 100.0f, true),
            f_asset("hdri_path", "textures", ".png"),
        }});
        add({"ReflectionProbe", "Lighting", {
            f_vec3("box_extent", glm::vec3(5.0f), 0.05f, true),
            f_int("resolution", 64, 8, 1024, true),
            f_float("blend_distance", 1.0f, 0.02f, 0.0f, 100.0f),
            f_int("importance", 0, -100, 100),
            f_float("intensity", 1.0f, 0.01f, 0.0f, 10.0f),
        }, false});
        add({"SdfRenderer", "Rendering", {
            f_vec3("bounds_center", glm::vec3(0.0f), 0.02f),
            f_vec3("bounds_extent", glm::vec3(1.0f), 0.02f, true),
            f_bool("cast_shadows", true),
            f_int("max_steps", 64, 1, 512),
            f_float("smoothing", 0.0f, 0.005f, 0.0f, 10.0f),
            f_material(),
        }});
        add({"SdfShape", "Rendering", {
            f_enum("shape", {"Sphere", "Box", "Plane"}, true),
            f_vec3("params", glm::vec3(0.5f), 0.01f, true),
            f_float("rounding", 0.0f, 0.005f, 0.0f, 10.0f),
            f_enum("op", {"Union", "Subtract", "Intersect"}),
            f_float("blend", 0.0f, 0.005f, 0.0f, 10.0f),
        }, false});
        add({"BoxCollider", "Physics", {
            f_vec3("size", glm::vec3(1.0f), 0.02f, true),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"SphereCollider", "Physics", {
            f_float("radius", 0.5f, 0.01f, 0.0f, 10000.0f, true),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"CapsuleCollider", "Physics", {
            f_float("radius", 0.5f, 0.01f, 0.0f, 10000.0f, true),
            f_float("height", 2.0f, 0.01f, 0.0f, 10000.0f, true),
            f_int("direction", 2, 0, 2),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"MeshCollider", "Physics", {
            f_asset("mesh_path", "meshes", ".yaml", true, true, "", true),
            f_bool("convex", false),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"Rigidbody", "Physics", {
            f_float("mass", 1.0f, 0.01f, 0.0f, 1e6f, true),
            f_float("drag", 0.0f, 0.005f, 0.0f, 100.0f),
            f_float("angular_drag", 0.05f, 0.005f, 0.0f, 100.0f),
            f_bool("use_gravity", true, true),
            f_bool("is_kinematic", false, true),
            f_bool("interpolation", false),
        }});
        add({"CameraController", "Gameplay", {
            f_enum("mode", {"Orbit", "Fly"}, true),
            f_vec3("target", glm::vec3(0.0f), 0.02f),
            f_float("distance", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_float("yaw_deg", 0.0f, 0.5f),
            f_float("pitch_deg", 30.0f, 0.5f, -89.0f, 89.0f),
            f_float("min_distance", 1.0f, 0.05f, 0.0f, 10000.0f),
            f_float("max_distance", 100.0f, 0.5f, 0.0f, 100000.0f),
            f_float("mouse_sensitivity", 0.2f, 0.005f, 0.0f, 10.0f),
            f_float("zoom_speed", 1.0f, 0.01f, 0.0f, 100.0f),
            f_bool("capture_cursor", false, true),
            f_float("auto_rotate_deg_per_sec", 0.0f, 0.1f),
            f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f),
            f_float("movement_smoothing", 0.0f, 0.005f, 0.0f, 1.0f),
        }});
        add({"KinematicMover", "Gameplay", {
            f_enum("mode", {"PingPong", "Orbit", "Spin"}, true),
            f_vec3("axis", glm::vec3(1.0f, 0.0f, 0.0f), 0.01f, true),
            f_float("distance", 2.0f, 0.02f),
            f_float("speed", 1.0f, 0.01f),
            f_vec3("orbit_center", glm::vec3(0.0f), 0.02f),
            f_float("orbit_radius", 2.0f, 0.02f),
            f_vec3("spin_axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f),
            f_float("spin_speed", 90.0f, 0.5f),
        }});
        add({"FreeMover", "Gameplay", {f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true), f_float("smoothing", 0.1f, 0.005f, 0.0f, 1.0f)}});
        add({"KinematicController", "Gameplay", {
            f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true), f_float("smoothing", 0.1f, 0.005f, 0.0f, 1.0f),
            f_bool("lock_height", true)}});
        add({"Terrain", "World", {
            f_int("seed", 1, 0, 1000000000, true),
            f_int("grid_size", 64, 4, 4096, true),
            f_float("sea_level", 0.3f, 0.005f, 0.0f, 1.0f),
            f_float("terrain_roughness", 0.5f, 0.005f, 0.0f, 1.0f),
            f_int("river_count", 4, 0, 1000),
            f_int("tiles_per_grid_unit", 1, 1, 64),
            f_float("tile_size", 1.0f, 0.01f, 0.01f, 100.0f),
            f_float("height_step", 0.25f, 0.005f, 0.01f, 100.0f),
            f_float("height_scale", 8.0f, 0.05f, 0.0f, 1000.0f),
            f_int("chunk_size", 32, 4, 512),
            f_float("view_radius", 64.0f, 0.5f, 1.0f, 100000.0f),
            f_bool("greedy_merge", true),
            f_asset("side_mesh", "meshes", ".yaml", true, true, "tile_side_flat"),
            f_material(),
        }});
        add({"Animator", "Animation", {f_bool("auto_play", true, true), f_float("speed", 1.0f, 0.01f), f_float("default_crossfade", 0.2f, 0.005f)}});
        return t;
    }();
    return table;
}

inline const ComponentSchema* find_schema(const std::string& type) {
    auto it = schemas().find(type);
    return it == schemas().end() ? nullptr : &it->second;
}

/** @brief A new component block of `type` holding each schema field marked in_default. */
inline Node default_component(const std::string& type) {
    Node c = Node::mapping();
    c["type"] = Node(type);
    const ComponentSchema* s = find_schema(type);
    if (!s) return c;
    for (const auto& f : s->fields) {
        if (!f.in_default) continue;
        switch (f.kind) {
            case FieldKind::Bool:  c[f.key] = Node(f.def.x != 0.0f); break;
            case FieldKind::Int:   c[f.key] = Node(static_cast<int64_t>(f.def.x)); break;
            case FieldKind::Float: c[f.key] = make_float(f.def.x); break;
            case FieldKind::Vec3:  c[f.key] = make_vec3(glm::vec3(f.def)); break;
            case FieldKind::Vec4:  { float v[4] = {f.def.x, f.def.y, f.def.z, f.def.w}; c[f.key] = make_float_seq(v, 4); break; }
            case FieldKind::Color: c[f.key] = make_color(glm::vec3(f.def)); break;
            case FieldKind::Enum:
            case FieldKind::String:
            case FieldKind::AssetRef:
                if (!f.default_string.empty()) c[f.key] = Node(f.default_string);
                break;
            case FieldKind::Material: {
                Node m = Node::mapping();
                m["albedo"] = make_color(glm::vec3(0.8f));
                m["metallic"] = make_float(0.0);
                m["roughness"] = make_float(0.5);
                c[f.key] = m;
                break;
            }
        }
    }
    return c;
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_COMPONENT_SCHEMA_H
