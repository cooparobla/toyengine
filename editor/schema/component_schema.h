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
#include <utility>
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
    Texture,    ///< An AssetRef into textures/, shown as a thumbnail you can drop a file onto.
    Curve,      ///< `[[x, y], ...]` in the unit square: a value over time, distance or life.
    Gradient,   ///< `[[t, r, g, b], ...]`: a colour over time, depth or life.
};

/**
 * @brief A condition on another key of the same block: the row, sub-heading or section it
 *        guards is drawn only while the condition holds.
 *
 * Values are compared as the inspector spells them: `true` / `false` for a Bool, the digits
 * for an Int (including an int-coded enum's index), the string for an Enum. A key the block
 * does not have is compared as its schema default, so a condition means the same thing before
 * and after the key is written.
 */
struct ShowWhen {
    std::string key;                   ///< Empty: always shown.
    std::vector<std::string> equals;   ///< Holds when the value is any of these.
    bool negate = false;               ///< Holds when it is none of them instead.

    bool always() const { return key.empty(); }
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
    std::string suffix;                ///< Unit appended to the number ("m", "Nm", "deg", "1/s").
    bool        slider = false;        ///< Bounded float drawn as a filled slider, not a drag.
    ShowWhen    show;                  ///< When this row is drawn at all.
    Node def_node;                     ///< Default for list kinds (StringList / ItemList); null: empty.
    std::vector<FieldDesc> item_fields;        ///< ItemList: the keys of one item.

    std::string display() const;
};

/** @brief A sub-heading inside a section, drawn before the section's `index`-th key. */
struct InspectorSubhead {
    size_t index = 0;
    std::string title;
    ShowWhen show;
};

/**
 * @brief One collapsible block of a component's inspector: which of its keys appear, in what
 *        order, under what heading, and when.
 *
 * A component with no sections is drawn as the flat list of all its fields, which is what
 * every component looked like before sections existed. Sections only decide what is SHOWN --
 * a key no section lists still appears (in a trailing "More" block) and is still saved, so a
 * wrong layout can hide a row but can never lose a value.
 */
struct InspectorSection {
    std::string title;
    std::string icon;                  ///< imm icon name (imm::icon_from_name); empty: none.
    std::string blurb;                 ///< One sentence under the header: what this controls.
    std::vector<std::string> keys;     ///< The schema fields drawn here, in this order.
    std::string toggle;                ///< A bool key drawn as the header's checkbox.
    ShowWhen show;                     ///< When the whole section is drawn.
    std::vector<InspectorSubhead> subheads;
    std::string custom;                ///< A registered custom drawer (inspector_sections.h), drawn first.
    bool open = true;                  ///< Open the first time it is shown.
};

struct ComponentSchema {
    std::string type;
    std::string category;              ///< "Rendering", "Physics", ... for the Add menu.
    std::vector<FieldDesc> fields;
    bool unique = true;                ///< At most one per object (Transform, Rigidbody...).
    bool removable = true;
    std::vector<InspectorSection> sections;   ///< Empty: the fields are drawn as one flat list.

    /** @brief The field named `key`, or null. */
    const FieldDesc* field(const std::string& key) const;
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
/** @brief A texture map: an AssetRef into textures/, drawn as a thumbnail slot. */
FieldDesc f_texture(std::string k, std::string label = {}, bool in_default = false);
/** @brief A curve over the unit square; `def` is the shape a fresh component gets. */
FieldDesc f_curve(std::string k, std::vector<glm::vec2> def = {{0.0f, 0.0f}, {1.0f, 1.0f}}, bool in_default = false);
/** @brief A colour ramp; `def` is the ramp a fresh component gets. */
FieldDesc f_gradient(std::string k, std::vector<std::pair<float, glm::vec3>> def = {}, bool in_default = false);

/** @brief The value a field is written with when it is added at its default. */
Node field_default_node(const FieldDesc& f);
FieldDesc with_label(FieldDesc f, std::string l);
FieldDesc listed(FieldDesc f);
FieldDesc root_relative(FieldDesc f);
FieldDesc startup(FieldDesc f);
FieldDesc project_only(FieldDesc f);
/** @brief An enum whose engine default is not its first option. */
FieldDesc with_default(FieldDesc f, std::string def);
/** @brief A row shown only while `key` holds one of `values` (see ShowWhen). */
FieldDesc shown_when(FieldDesc f, std::string key, std::vector<std::string> values);
/** @brief A row shown only while `key` holds NONE of `values`. */
FieldDesc hidden_when(FieldDesc f, std::string key, std::vector<std::string> values);
/** @brief A bounded float drawn as a filled slider instead of a drag. */
FieldDesc as_slider(FieldDesc f);
/** @brief The unit shown after the number ("m", "Nm", "deg"). */
FieldDesc with_unit(FieldDesc f, std::string unit);

/**
 * @brief Builds one InspectorSection, so a layout table reads as prose.
 * @code
 * sec("Steering", "Tool").says("How far and how fast the wheels turn.")
 *     .keys({"steer_mode", "max_steer_deg", "min_steer_deg"})
 *     .sub("At speed", 2).when("steer_mode", {"ackermann"}).done()
 * @endcode
 */
class SectionBuilder {
public:
    SectionBuilder(std::string title, std::string icon = {});
    /** @brief The one-sentence explanation under the header. */
    SectionBuilder& says(std::string blurb);
    /** @brief Adds keys, in order; call it more than once to interleave with sub(). */
    SectionBuilder& keys(std::vector<std::string> ks);
    /** @brief A sub-heading before the keys added next, optionally conditional. */
    SectionBuilder& sub(std::string title, std::string show_key = {}, std::vector<std::string> values = {});
    /** @brief A bool key drawn as the header's checkbox. */
    SectionBuilder& toggle(std::string key);
    /** @brief Draws the whole section only while `key` holds one of `values`. */
    SectionBuilder& when(std::string key, std::vector<std::string> values);
    /** @brief A registered custom drawer (inspector_sections.h) drawn above the rows. */
    SectionBuilder& custom(std::string id);
    /** @brief Starts collapsed. */
    SectionBuilder& closed();
    InspectorSection done();

private:
    InspectorSection s_;
};

/** @brief Shorthand for SectionBuilder{title, icon}. */
SectionBuilder sec(std::string title, std::string icon = {});

/** @brief Gives each built-in schema its inspector layout; called once, from schema_table_(). */
void apply_component_sections(std::map<std::string, ComponentSchema>& table);

/** @brief The keys of a PBRMaterial block (inline, or a materials/*.yaml asset). */
const std::vector<FieldDesc>& material_fields();

/** @brief How those keys are grouped in the inspector: Surface, Maps, Transparency... */
const std::vector<InspectorSection>& material_sections();

/**
 * @brief A material as a schema, so the inspector draws it with exactly the same section code
 *        as a component -- conditions, sub-headings and all.
 */
const ComponentSchema& material_schema();

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
