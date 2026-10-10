/**
 * @file inspector.h
 * @brief Draws editable property rows for a YAML block (a component, a material, a config
 *        section) with uicoopa's immediate-mode widgets, guided by a schema.
 *
 * Edits are applied to the node in place; the caller decides what an edit means (push an
 * undo step, rebuild a live object, mark a file dirty). EditResult says whether anything
 * changed this frame, whether a continuous edit (a drag) is still in progress -- so the
 * caller can merge it into one undo step -- and when it finished.
 */

#ifndef TOYEDITOR_SCHEMA_INSPECTOR_H
#define TOYEDITOR_SCHEMA_INSPECTOR_H

#include "component_schema.h"

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

namespace imm = coopa::ui::imm;

/** @brief What the inspector needs from the editor around it. */
struct InspectorEnv {
    /// Project-relative asset paths ("meshes/cube.yaml") under `dir` with extension `ext`.
    std::function<std::vector<std::string>(const std::string& dir, const std::string& ext)> list_assets;
    /// Names a ChildRef field may pick (the edited object's descendants, then other objects).
    std::function<std::vector<std::string>()> object_names;
    /// Called after each field is drawn, with its key and its label's box: the Components tab
    /// marks a prefab instance's overridden fields there and gives them a right-click menu.
    std::function<void(imm::Context&, const std::string& key, const imm::Box& label)> after_field;
};

struct EditResult {
    bool changed = false;     ///< The node was modified this frame.
    bool active = false;      ///< A drag/edit is still in progress (merge undo steps).
    bool finished = false;    ///< A drag/edit ended this frame (close the undo merge).
    std::string key;          ///< The key that changed (for undo labels / merge keys).

    void absorb(const EditResult& o);
};

namespace detail {

inline bool is_numeric(const Node& n) { return n.is_integer() || n.is_float_number(); }

bool has_numeric_keys(const Node& n, std::initializer_list<const char*> ks);

/** @brief Value of an AssetRef as stored -> the project-relative path it denotes. */
std::string ref_to_path(const FieldDesc& f, const std::string& v);

/** @brief Project-relative path -> the value an AssetRef field stores. */
std::string path_to_ref(const FieldDesc& f, const std::string& path);

void finish(imm::Context& ctx, EditResult& r, const std::string& key, bool changed, bool group = false);

} // namespace detail

/** @brief One field of `block` (the key may be absent: the schema default is shown). */
EditResult draw_field(imm::Context& ctx, const FieldDesc& f, Node& block, const InspectorEnv& env);

/** @brief A generic row for a key no schema describes, inferred from its value. */
EditResult draw_generic(imm::Context& ctx, const std::string& key, Node& block);

/**
 * @brief Draws every schema field of `block`, then (optionally) any extra keys generically.
 * @param skip Keys never shown (e.g. "type").
 */
EditResult draw_fields(imm::Context& ctx, const std::vector<FieldDesc>& fields, Node& block,
                              const InspectorEnv& env, bool show_unknown = true,
                              const std::set<std::string>& skip = {"type", "id"});

/**
 * @brief A material's Shader section: which shader it renders with, and that shader's own
 *        shader_params under their names. Choosing a shader keeps alpha_mode in the shader's
 *        pass (a Transparent shader is only drawn for BLEND, an Opaque one never is -- the
 *        renderer would silently fall back to the stock shader) and resets the params to the
 *        new shader's defaults.
 */
EditResult draw_shader_section(imm::Context& ctx, Node& m);

/** @brief Every field of a material block: the Shader section first, then the PBR fields. */
EditResult draw_material_block(imm::Context& ctx, Node& m, const InspectorEnv& env, bool show_unknown = true);

/**
 * @brief The `material:` key of a renderer component: Inline / Asset / Asset + overrides.
 * @param open_material Called with a project-relative material path when "Edit" is clicked.
 */
EditResult draw_material_field(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                      const std::function<void(const std::string&)>& open_material = {});

/**
 * @brief The full inspector body for one component node.
 * @return The edit result; `comp` is modified in place.
 */
EditResult draw_component(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                 const std::function<void(const std::string&)>& open_material = {});

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_INSPECTOR_H
