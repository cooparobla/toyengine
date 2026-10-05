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
    // --- UI (uicoopa's YAML spellings) ---
    Vec2,       ///< `{x, y}`.
    Color4,     ///< `{r, g, b, a}` -- colour with alpha.
    Padding,    ///< `{left, right, top, bottom}`.
    StringList, ///< A sequence of strings (ComboBox items, TabView tabs).
    ChildRef,   ///< The NAME of another object (usually a descendant): a dropdown of names.
    ItemList,   ///< A sequence of small maps described by `item_fields` (MenuList buttons).
};

struct FieldDesc {
    std::string key;
    FieldKind   kind = FieldKind::Float;
    std::string label;                 ///< Empty: derived from the key.
    float       speed = 0.01f;         ///< Drag speed for numbers.
    float       min = -1e30f, max = 1e30f;
    glm::vec4   def{0.0f};             ///< Default (x for scalars, xyz vectors/colours).
    std::vector<std::string> options;  ///< Enum values; for Int, labels for 0, 1, 2... (a labelled int enum).
    std::vector<std::string> option_tips;  ///< Per-option explanation, shown as the field's tooltip.
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
    Node def_node;                     ///< Default for list kinds (StringList / ItemList); null: empty.
    std::vector<FieldDesc> item_fields;        ///< ItemList: the keys of one item.

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
/**
 * @brief An int-coded mode (`fog_mode: 2`) shown as a dropdown of `labels`; the file still
 *        stores the index, which is what the engine parses.
 */
inline FieldDesc f_int_enum(std::string k, std::vector<std::string> labels, int def,
                            std::vector<std::string> tips = {}, bool in_default = false) {
    FieldDesc f = f_int(std::move(k), def, 0, static_cast<int>(labels.size()) - 1, in_default);
    f.options = std::move(labels); f.option_tips = std::move(tips); return f;
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
inline FieldDesc f_vec2(std::string k, glm::vec2 def, float speed = 1.0f, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Vec2; f.def = glm::vec4(def, 0.0f, 0.0f); f.speed = speed; f.in_default = in_default; return f;
}
inline FieldDesc f_color4(std::string k, glm::vec4 def, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Color4; f.def = def; f.in_default = in_default; return f;
}
inline FieldDesc f_padding(std::string k, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Padding; f.def = glm::vec4(0.0f); f.speed = 0.5f; f.in_default = in_default; return f;
}
inline FieldDesc f_strings(std::string k, std::vector<std::string> def = {}, bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::StringList; f.in_default = in_default;
    f.def_node = Node::sequence();
    for (auto& d : def) f.def_node.as_seq().push_back(Node(d));
    return f;
}
inline FieldDesc f_child(std::string k, std::string def = "", bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::ChildRef; f.default_string = std::move(def); f.in_default = in_default; return f;
}
/** @brief A list of items, each a map of `item` fields; `def` is the starting list. */
inline FieldDesc f_items(std::string k, std::vector<FieldDesc> item, Node def = Node::sequence(), bool in_default = false) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::ItemList; f.item_fields = std::move(item); f.def_node = std::move(def);
    f.in_default = in_default; return f;
}
inline FieldDesc with_tip(FieldDesc f, std::string tip) { f.tooltip = std::move(tip); return f; }

/** @brief The value a field is written with when it is added at its default. */
inline Node field_default_node(const FieldDesc& f) {
    switch (f.kind) {
        case FieldKind::Bool:  return Node(f.def.x != 0.0f);
        case FieldKind::Int:   return Node(static_cast<int64_t>(f.def.x));
        case FieldKind::Float: return make_float(f.def.x);
        case FieldKind::Vec3:  return make_vec3(glm::vec3(f.def));
        case FieldKind::Vec4:  { float v[4] = {f.def.x, f.def.y, f.def.z, f.def.w}; return make_float_seq(v, 4); }
        case FieldKind::Color: return make_color(glm::vec3(f.def));
        case FieldKind::Vec2: {
            Node n = Node::mapping(); n["x"] = make_float(f.def.x); n["y"] = make_float(f.def.y); return n;
        }
        case FieldKind::Color4: {
            if (f.as_list) { float v[4] = {f.def.r, f.def.g, f.def.b, f.def.a}; return make_float_seq(v, 4); }
            Node n = make_color(glm::vec3(f.def)); n["a"] = make_float(f.def.w); return n;
        }
        case FieldKind::Padding: {
            Node n = Node::mapping();
            n["left"] = make_float(f.def.x); n["right"] = make_float(f.def.y); n["top"] = make_float(f.def.z); n["bottom"] = make_float(f.def.w);
            return n;
        }
        case FieldKind::StringList:
        case FieldKind::ItemList: return f.def_node.is_sequence() ? f.def_node : Node::sequence();
        case FieldKind::Enum:
        case FieldKind::String:
        case FieldKind::AssetRef:
        case FieldKind::ChildRef: return Node(f.default_string);
        case FieldKind::Material: {
            Node m = Node::mapping();
            m["albedo"] = make_color(glm::vec3(0.8f));
            m["metallic"] = make_float(0.0);
            m["roughness"] = make_float(0.5);
            return m;
        }
    }
    return Node();
}
inline FieldDesc with_label(FieldDesc f, std::string l) { f.label = std::move(l); return f; }
inline FieldDesc listed(FieldDesc f) { f.as_list = true; return f; }
inline FieldDesc root_relative(FieldDesc f) { f.ref_prefix = "assets/"; return f; }
inline FieldDesc startup(FieldDesc f) { f.startup_only = true; return f; }
/** @brief An enum whose engine default is not its first option. */
inline FieldDesc with_default(FieldDesc f, std::string def) { f.default_string = std::move(def); return f; }

/** @brief The keys of a PBRMaterial block (inline, or a materials/*.yaml asset). */
inline const std::vector<FieldDesc>& material_fields() {
    static const std::vector<FieldDesc> fields = {
        f_color("albedo", glm::vec3(0.8f), true),
        f_float("metallic", 0.0f, 0.005f, 0.0f, 1.0f, true),
        f_float("roughness", 0.5f, 0.005f, 0.0f, 1.0f, true),
        f_float("ao", 1.0f, 0.005f, 0.0f, 1.0f),
        f_color("emissive", glm::vec3(0.0f)),
        f_float("emissive_strength", 1.0f, 0.02f, 0.0f, 1000.0f),
        f_enum("alpha_mode", {"OPAQUE", "MASK", "CUTOUT", "BLEND"}),
        f_float("alpha", 1.0f, 0.005f, 0.0f, 1.0f),
        f_float("alpha_cutoff", 0.5f, 0.005f, 0.0f, 1.0f),
        f_bool("cull_backfaces", true),
        f_bool("refraction", false),
        with_tip(f_float("ior", -1.0f, 0.005f, -1.0f, 3.0f), "-1: the render settings' refraction_ior (glass ~1.45, water ~1.33)"),
        with_tip(f_float("refraction_thickness", -1.0f, 0.005f, -1.0f, 10.0f), "-1: the render settings' refraction_thickness"),
        f_color("refraction_tint", glm::vec3(1.0f)),
        with_label(f_asset("texture_albedo", "textures", ".png"), "Albedo map"),
        with_label(f_asset("texture_normal", "textures", ".png"), "Normal map"),
        with_label(f_asset("texture_metallic_roughness", "textures", ".png"), "Metal/rough map"),
        with_label(f_asset("texture_alpha_mask", "textures", ".png"), "Alpha mask"),
        // Drawn by draw_material_block()'s Shader section (named per-shader params), not as
        // plain fields -- listed here so they count as known material keys and overrides.
        f_enum("shader", {"", "triplanar", "foliage", "water"}),
        [] { FieldDesc f; f.key = "shader_params"; f.kind = FieldKind::Vec4; f.speed = 0.01f; return f; }(),
    };
    return fields;
}

/** @brief One slot of a surface shader's `shader_params`, as the material editor labels it. */
struct ShaderParamDesc {
    std::string label;
    float def = 0.0f, min = 0.0f, max = 0.0f, speed = 0.01f;
    std::vector<std::string> options;   ///< Non-empty: a dropdown whose index is the value.
};

/**
 * @brief A shader a material can choose: the stock PBR one ("") or a surface shader the engine
 *        registers in toyengine/core/engine.h's make_render_config_(). Engine-internal ones
 *        (terrain, editor_paint) are left out; the editor tests check every name here is
 *        registered.
 */
struct SurfaceShaderInfo {
    std::string name;
    std::string label;
    bool transparent = false;   ///< Transparent domain: only drawn for alpha_mode BLEND.
    std::string note;           ///< One line under the dropdown.
    bool driven = false;        ///< shader_params are written by a component at runtime.
    std::vector<ShaderParamDesc> params;   ///< shader_params[0..3], in order.
};

inline const std::vector<SurfaceShaderInfo>& surface_shaders() {
    static const std::vector<SurfaceShaderInfo> shaders = {
        {"", "Standard (PBR)", false, "Deferred PBR, mesh UVs. Every material uses it unless it picks another.", false, {}},
        {"triplanar", "Triplanar", false, "Maps projected along X/Y/Z instead of mesh UVs. Opaque only.", false, {
            {"Tiling", 1.0f, 0.01f, 100.0f, 0.01f, {}},
            {"Blend sharpness", 4.0f, 1.0f, 32.0f, 0.05f, {}},
            {"Space", 0.0f, 0.0f, 1.0f, 1.0f, {"World", "Object"}},
            {"Normal strength", 1.0f, 0.0f, 4.0f, 0.01f, {}},
        }},
        {"foliage", "Foliage", false, "Wind-swayed two-sided card; pair with CUTOUT + an alpha mask.", false, {
            {"Wind strength", 0.12f, 0.0f, 10.0f, 0.005f, {}},
            {"Wind frequency", 1.6f, 0.0f, 50.0f, 0.01f, {}},
            {"Wind dir X", 1.0f, -1.0f, 1.0f, 0.01f, {}},
            {"Wind dir Y", 0.35f, -1.0f, 1.0f, 0.01f, {}},
        }},
        {"water", "Water", true, "Waves, depth colour and foam. Needs a WaterBody on the object.", true, {}},
    };
    return shaders;
}

inline const SurfaceShaderInfo* find_surface_shader(const std::string& name) {
    for (const auto& s : surface_shaders()) if (s.name == name) return &s;
    return nullptr;
}

} // namespace toy::editor

#include "ui_schema.h"   // the UI components' schemas (add_ui_schemas()); needs the builders above

namespace toy::editor {

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
            with_tip(f_float("lod_bias", 1.0f, 0.01f, 0.0f, 100.0f), "Multiplier on the LOD switch distances (1 = as authored)"),
            f_bool("lods_enabled", true),
            f_bool("affects_reflection_probes", true),
        }});
        add({"Camera", "Rendering", {
            f_bool("main", true, true),
            f_enum("projection", {"Perspective", "Orthographic"}, true),
            f_float("fov", 60.0f, 0.2f, 1.0f, 179.0f, true),
            f_float("orthographic_size", 3.0f, 0.05f, 0.01f, 10000.0f),
            f_float("near_clip_plane", 0.1f, 0.01f, 0.0001f, 1000.0f, true),
            f_float("far_clip_plane", 1000.0f, 1.0f, 0.01f, 100000.0f, true),
            f_float("lens", 50.0f, 0.5f, 1.0f, 1000.0f),
            with_tip(f_float("aperture", 0.0f, 0.05f, 0.0f, 64.0f), "f-stop; 0 uses the render settings' dof_aperture"),
            with_tip(f_float("focus_distance", 0.0f, 0.05f, 0.0f, 10000.0f), "Metres; 0 uses the render settings' dof_focus_distance"),
            f_string("focus_object"),
        }});
        add({"DirectionalLight", "Lighting", {
            f_vec3("direction", glm::vec3(-0.35f, -0.45f, -0.82f), 0.01f, true),
            f_color("color", glm::vec3(1.0f, 0.97f, 0.9f), true),
            f_float("intensity", 1.0f, 0.01f, 0.0f, 1000.0f, true),
            f_bool("cast_shadows", true, true),
            f_float("shadow_intensity", 1.0f, 0.01f, 0.0f, 1.0f),
        }});
        add({"PointLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_bool("cast_shadows", false, true),
            f_float("attenuation_constant", 1.0f, 0.05f, 0.1f, 64.0f),
        }, false});
        add({"SpotLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_vec3("direction", glm::vec3(0.0f, 0.0f, -1.0f), 0.01f, true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_float("inner_angle", 20.0f, 0.2f, 0.0f, 89.0f, true),
            f_float("outer_angle", 30.0f, 0.2f, 0.0f, 89.0f, true),
            f_bool("cast_shadows", true),
            f_float("attenuation_constant", 1.0f, 0.05f, 0.1f, 64.0f),
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
            f_int("importance", 1, -100, 100),
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
            f_int_enum("direction", {"X", "Y", "Z"}, 2,
                       {"Capsule axis along local X", "Capsule axis along local Y", "Capsule axis along local Z (up)"}),
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
            f_enum("interpolation", {"Interpolate", "None"}),   // physx_yaml.h reads a string: "None" or anything else
        }});
        // toyengine/water/: a body of water (baked by WaterSystem onto the sibling MeshRenderer)
        // and a floating Rigidbody. `size` uses x/y only (the procedural grid's extent).
        add({"WaterBody", "Water", {
            f_enum("mode", {"planar", "flowing"}, true),
            f_asset("mesh_path", "meshes", ".yaml", true, true),
            f_vec3("size", glm::vec3(20.0f, 20.0f, 0.0f), 0.1f, true),
            f_int("resolution", 48, 1, 512, true),
            f_float("tile_size", 0.0f, 0.5f, 0.0f, 1000.0f),
            f_float("wave_amplitude", 0.15f, 0.005f, 0.0f, 20.0f, true),
            f_float("wave_length", 8.0f, 0.05f, 0.1f, 500.0f, true),
            f_float("wave_direction", 0.0f, 0.5f, -360.0f, 360.0f),
            f_float("wave_steepness", 0.5f, 0.005f, 0.0f, 1.0f),
            f_float("flow_speed", 1.0f, 0.01f, 0.0f, 50.0f),
            f_float("flow_min_speed", 0.6f, 0.01f, 0.0f, 50.0f),
            f_float("flow_slope_gain", 4.0f, 0.05f, 0.0f, 100.0f),
            f_float("obstacle_radius", 1.5f, 0.02f, 0.0f, 50.0f),
            f_float("wake_length", 4.0f, 0.05f, 0.0f, 100.0f),
            f_color("foam_color", glm::vec3(0.92f, 0.96f, 1.0f)),
            f_float("foam_amount", 1.0f, 0.01f, 0.0f, 4.0f),
            f_float("shore_foam_depth", 0.6f, 0.01f, 0.0f, 20.0f),
            f_float("edge_fade_depth", 0.15f, 0.005f, 0.0f, 5.0f),
            f_float("ripple_strength", 0.35f, 0.005f, 0.0f, 4.0f),
            f_float("ripple_scale", 1.2f, 0.01f, 0.01f, 20.0f),
            f_float("clarity", 3.0f, 0.02f, 0.01f, 200.0f),
            f_color("underwater_color", glm::vec3(0.05f, 0.24f, 0.28f)),
            f_float("underwater_visibility", 14.0f, 0.1f, 0.1f, 1000.0f),
            f_color("underwater_absorption", glm::vec3(0.35f, 0.09f, 0.06f)),
            f_float("caustics", 1.0f, 0.01f, 0.0f, 10.0f),
            f_float("density", 1000.0f, 1.0f, 1.0f, 20000.0f),
            f_float("max_depth", 60.0f, 0.5f, 0.1f, 10000.0f),
        }});
        add({"Buoyancy", "Water", {
            f_int("subdivisions", 2, 1, 4, true),
            f_float("volume", -1.0f, 0.01f, -1.0f, 1e6f),
            f_float("linear_drag", 1.5f, 0.01f, 0.0f, 100.0f, true),
            f_float("angular_drag", 1.0f, 0.01f, 0.0f, 100.0f, true),
            f_float("form_drag", 1.0f, 0.01f, 0.0f, 10.0f),
            f_float("buoyancy_scale", 1.0f, 0.01f, 0.0f, 10.0f),
        }});
        add({"CameraController", "Gameplay", {
            f_enum("mode", {"Orbit", "Fly"}, true),
            f_vec3("target", glm::vec3(0.0f), 0.02f),
            with_tip(f_float("distance", -1.0f, 0.05f, -1.0f, 10000.0f), "-1: from the camera's placement relative to the target"),
            with_tip(f_float("yaw_deg", 0.0f, 0.5f), "Unset: from the camera's placement relative to the target"),
            with_tip(f_float("pitch_deg", 0.0f, 0.5f, -89.0f, 89.0f), "Unset: from the camera's placement relative to the target"),
            f_float("min_distance", 0.5f, 0.05f, 0.0f, 10000.0f),
            f_float("max_distance", 100.0f, 0.5f, 0.0f, 100000.0f),
            f_float("mouse_sensitivity", 0.15f, 0.005f, 0.0f, 10.0f),
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
        add({"FreeMover", "Gameplay", {f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true),
                                       with_tip(f_float("smoothing", 12.0f, 0.1f, 0.0f, 100.0f), "Follow rate (1/s); 0 snaps")}});
        add({"KinematicController", "Gameplay", {
            f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true),
            with_tip(f_float("smoothing", 10.0f, 0.1f, 0.0f, 100.0f), "Follow rate (1/s); 0 snaps"),
            f_bool("lock_height", true)}});
        // Defaults mirror the runtime's (toyengine/world/terrain_component.h, terrain_sampler.h):
        // an absent key shows the value the terrain actually uses.
        add({"Terrain", "World", {
            f_int("seed", 251, 0, 1000000000, true),
            f_int("grid_size", 64, 4, 4096, true),
            f_float("sea_level", 0.25f, 0.005f, 0.0f, 1.0f),
            f_float("terrain_roughness", 0.35f, 0.005f, 0.0f, 1.0f),
            f_int("river_count", 25, 0, 1000),
            f_int("tiles_per_grid_unit", 4, 1, 64),
            f_float("tile_size", 1.0f, 0.01f, 0.01f, 100.0f),
            f_float("height_step", 1.0f, 0.005f, 0.01f, 100.0f),
            f_float("height_scale", 40.0f, 0.05f, 0.0f, 1000.0f),
            f_int("chunk_size", 16, 4, 512),
            with_label(f_int("view_radius", 3, 0, 64), "View radius (chunks)"),
            f_int("max_wall_steps", 24, 0, 1024),
            f_int("soil_depth_steps", 3, 0, 1024),
            f_bool("greedy_merge", true),
            f_bool("emit_bottom", false),
            f_int("max_chunk_jobs_per_frame", 4, 1, 64),
            f_asset("side_mesh", "meshes", ".yaml", true, true, "tile_side_flat"),
            f_material(),
        }});
        // A rig root: its clips are `states` (managed in the Timeline -- one file each under
        // animations/<object>/), `auto_play` names the state played on start.
        add({"Animator", "Animation", {f_string("auto_play", "", true), f_float("speed", 1.0f, 0.01f, 0.0f, 100.0f),
                                       f_float("default_crossfade", 0.0f, 0.005f, 0.0f, 10.0f)}});
        // A mesh deformed by a rig's bones: `bones:` (or, omitted, the mesh's vertex groups)
        // resolved under `rig:` (default: the nearest Animator up the hierarchy).
        add({"SkinnedMeshRenderer", "Rendering", {f_asset("mesh_path", "meshes", ".yaml", true, true, "", true), f_string("rig", "")}});
        add_ui_schemas(add);
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
        const bool stringish = f.kind == FieldKind::Enum || f.kind == FieldKind::String || f.kind == FieldKind::AssetRef ||
                               f.kind == FieldKind::ChildRef;
        if (stringish && f.default_string.empty()) continue;
        c[f.key] = field_default_node(f);
    }
    return c;
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_COMPONENT_SCHEMA_H
