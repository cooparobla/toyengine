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
    bool        startup_only = false;  ///< Render settings: fixed at pipeline construction; a change rebuilds the renderer.
    bool        tier_driven = false;   ///< Render settings: a quality tier sets it when config.yaml doesn't (no fixed default).
    bool        project_only = false;  ///< Render settings: baked in at engine start-up, so config.yaml only (no scene override).
    std::string default_string;
    std::string tooltip;
    Node def_node;                     ///< Default for list kinds (StringList / ItemList); null: empty.
    std::vector<FieldDesc> item_fields;        ///< ItemList: the keys of one item.

    std::string display() const;
};

struct ComponentSchema {
    std::string type;
    std::string category;              ///< "Rendering", "Physics", ... for the Add menu.
    std::vector<FieldDesc> fields;
    bool unique = true;                ///< At most one per object (Transform, Rigidbody...).
    bool removable = true;
};

// --- field builders (keep the tables below readable) ---
FieldDesc f_bool(std::string k, bool def, bool in_default = false);
FieldDesc f_int(std::string k, int def, int lo = -1000000, int hi = 1000000, bool in_default = false);
FieldDesc f_float(std::string k, float def, float speed = 0.01f, float lo = -1e30f, float hi = 1e30f, bool in_default = false);
FieldDesc f_vec3(std::string k, glm::vec3 def, float speed = 0.01f, bool in_default = false);
FieldDesc f_color(std::string k, glm::vec3 def, bool in_default = false);
FieldDesc f_enum(std::string k, std::vector<std::string> opts, bool in_default = false);
/**
 * @brief An int-coded mode (`fog_mode: 2`) shown as a dropdown of `labels`; the file still
 *        stores the index, which is what the engine parses.
 */
FieldDesc f_int_enum(std::string k, std::vector<std::string> labels, int def,
                            std::vector<std::string> tips = {}, bool in_default = false);
FieldDesc f_string(std::string k, std::string def = "", bool in_default = false);
FieldDesc f_asset(std::string k, std::string dir, std::string ext, bool strip_ext = false, bool strip_dir = false,
                         std::string def = "", bool in_default = false);
FieldDesc f_material();
FieldDesc f_vec2(std::string k, glm::vec2 def, float speed = 1.0f, bool in_default = false);
FieldDesc f_color4(std::string k, glm::vec4 def, bool in_default = false);
FieldDesc f_padding(std::string k, bool in_default = false);
FieldDesc f_strings(std::string k, std::vector<std::string> def = {}, bool in_default = false);
FieldDesc f_child(std::string k, std::string def = "", bool in_default = false);
/** @brief A list of items, each a map of `item` fields; `def` is the starting list. */
FieldDesc f_items(std::string k, std::vector<FieldDesc> item, Node def = Node::sequence(), bool in_default = false);
FieldDesc with_tip(FieldDesc f, std::string tip);

/** @brief The value a field is written with when it is added at its default. */
Node field_default_node(const FieldDesc& f);
FieldDesc with_label(FieldDesc f, std::string l);
FieldDesc listed(FieldDesc f);
FieldDesc root_relative(FieldDesc f);
FieldDesc startup(FieldDesc f);
FieldDesc project_only(FieldDesc f);
/** @brief An enum whose engine default is not its first option. */
FieldDesc with_default(FieldDesc f, std::string def);

/** @brief The keys of a PBRMaterial block (inline, or a materials/*.yaml asset). */
const std::vector<FieldDesc>& material_fields();

/** @brief One slot of a surface shader's `shader_params`, as the material editor labels it. */
struct ShaderParamDesc {
    std::string label;
    float def = 0.0f, min = 0.0f, max = 0.0f, speed = 0.01f;
    std::vector<std::string> options;   ///< Non-empty: a dropdown whose index is the value.
};

/**
 * @brief A shader a material can choose: the stock PBR one ("") or a surface shader the engine
 *        registers in toyengine/core/engine.h's make_render_config_(). Engine-internal ones
 *        (terrain, terrain_styled, editor_paint) are left out; the editor tests check every name here is
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

const std::vector<SurfaceShaderInfo>& surface_shaders();

const SurfaceShaderInfo* find_surface_shader(const std::string& name);

} // namespace toy::editor

#include "ui_schema.h"   // the UI components' schemas (add_ui_schemas()); needs the builders above

namespace toy::editor {

/** @brief The schema table: every built-in schema, plus any register_component_schema() added. */
std::map<std::string, ComponentSchema>& schema_table_();

/** @brief Every component schema, keyed by type name. */
inline const std::map<std::string, ComponentSchema>& schemas() { return schema_table_(); }

/**
 * @brief Adds (or replaces) a component schema -- how a game project describes its own
 *        components to the inspector and "Add Component". Call it from a project module under
 *        `#if TOY_EDITOR` (toyengine_add_project() defines that for the editor build only):
 * @code
 * #if TOY_EDITOR
 * #include <editor/schema/component_schema.h>
 * static const bool spinner_schema = toy::editor::register_component_schema(
 *     {"Spinner", "Gameplay", {toy::editor::f_vec3("axis", {0, 0, 1}, 0.01f, true)}});
 * #endif
 * @endcode
 * @return true, so it can initialise a static.
 */
bool register_component_schema(ComponentSchema schema);

const ComponentSchema* find_schema(const std::string& type);

/** @brief A new component block of `type` holding each schema field marked in_default. */
Node default_component(const std::string& type);

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_COMPONENT_SCHEMA_H
