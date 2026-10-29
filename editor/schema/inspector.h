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
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace coopa::scene { class SceneObject; }

namespace toy::editor {

namespace imm = coopa::ui::imm;

/// As editor/core/scene_document.h spells it; repeated so this header stays off that one.
using ObjectId = int64_t;

/** @brief What the inspector needs from the editor around it. */
struct InspectorEnv {
    /// Project-relative asset paths ("meshes/cube.yaml") under `dir` with extension `ext`.
    std::function<std::vector<std::string>(const std::string& dir, const std::string& ext)> list_assets;
    /// The name of the object with this id, or empty. Lets a ChildRef field accept an object
    /// dragged out of the outliner, whose drag payload is an id rather than a name.
    std::function<std::string(ObjectId)> object_name;
    /// Names a ChildRef field may pick (the edited object's descendants, then other objects).
    std::function<std::vector<std::string>()> object_names;
    /// Called after each field is drawn, with its key and its label's box: the Components tab
    /// marks a prefab instance's overridden fields there and gives them a right-click menu.
    std::function<void(imm::Context&, const std::string& key, const imm::Box& label)> after_field;
    /// The live object the inspected block belongs to, or null: a custom section reads runtime
    /// state through it (particle counts, a vehicle's speed) and drives previews.
    std::function<coopa::scene::SceneObject*()> live_object;
    /// Project-relative asset path ("graphs/stair.yaml") -> the absolute file that resolves,
    /// searching the project's assets, then toyengine's, then by name. Empty when unset (tests)
    /// or when nothing matches. A custom section that must READ an asset to know what rows to
    /// draw -- a node graph's interface decides the ProceduralGraph inspector -- needs this.
    std::function<std::string(const std::string& rel)> asset_path;
    /// Opens a project-relative asset ("materials/brick.yaml") in the editor.
    std::function<void(const std::string& path)> open_asset;
    /// A thumbnail of the image at a project-relative path ("textures/brick.png"), or a null
    /// view while it is not loaded. Unset (tests, headless): texture slots fall back to a
    /// plain file dropdown.
    std::function<coopa::gfx::TextureView(const std::string& path)> texture_preview;
    /// The object being inspected, for a custom section that needs to name it. 0 outside a scene.
    ObjectId object_id = 0;
    /// The game is running: a custom section must not offer edits that only make sense stopped.
    bool playing = false;
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

/**
 * @brief The value of `key` as a ShowWhen compares it: "true"/"false" for a bool, the digits
 *        for an int, the text for a string or enum. A key the block lacks reads as its schema
 *        default, so a condition means the same before and after the key is written.
 */
std::string condition_value(const Node& block, const std::string& key, const std::vector<FieldDesc>& fields);

/** @brief Whether the row, sub-heading or section `w` guards should be drawn. */
bool show_when_holds(const Node& block, const ShowWhen& w, const std::vector<FieldDesc>& fields);

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
 * @brief One collapsible section of a component: its header (with icon, blurb and optional
 *        toggle), its custom drawer if it names one, then its rows.
 * @param drawn Keys already drawn, added to as this section draws its own -- including the
 *              keys of a section that is closed or whose condition fails, so they are not
 *              repeated in the trailing "More" block.
 */
EditResult draw_section(imm::Context& ctx, const InspectorSection& s, const ComponentSchema& schema, Node& comp,
                        const InspectorEnv& env, std::set<std::string>& drawn,
                        const std::function<void(const std::string&)>& open_material = {});

/**
 * @brief The full inspector body for one component node.
 * @param hide_keys Keys the caller draws itself (the Components tab keeps a MeshRenderer's
 *                  material on its own tab); they are skipped, not pushed down into "More".
 * @return The edit result; `comp` is modified in place.
 */
EditResult draw_component(imm::Context& ctx, Node& comp, const InspectorEnv& env,
                                 const std::function<void(const std::string&)>& open_material = {},
                                 const std::set<std::string>* hide_keys = nullptr);

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_INSPECTOR_H
