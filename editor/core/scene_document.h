/**
 * @file scene_document.h
 * @brief The editor's model of one scene file: its YAML document, object ids and undo.
 *
 * The document IS the source of truth. The live coopa::scene::Scene the viewport renders
 * is always derived from it (SceneLoader::load_from_node()), never the other way round, so:
 *
 *   - saving is lossless for everything the editor does not understand -- a component type
 *     with no inspector schema, an unknown key, an `inherit_from` -- because the node holds
 *     it verbatim;
 *   - undo is exact (whole-document snapshots, see undo.h);
 *   - play mode is "build another Scene from the same node".
 *
 * Every object node carries a private `__eid` integer key, stamped on load and on creation.
 * SceneLoader ignores unknown keys, and an object observer (see SceneSync) uses the stamp
 * to map document objects to the live SceneObjects built from them. strip_private_keys()
 * removes the stamps when saving.
 */

#ifndef TOYEDITOR_CORE_SCENE_DOCUMENT_H
#define TOYEDITOR_CORE_SCENE_DOCUMENT_H

#include "undo.h"
#include "yaml_util.h"

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

using ObjectId = int64_t;
inline constexpr const char* kEidKey = "__eid";

/** @brief How far an edit's effect reaches, so the live scene is rebuilt no more than needed. */
enum class ChangeScope {
    None,
    Transform,    ///< Only Transform values of `object` changed: patch the live transform.
    Rect,         ///< Only RectTransform values of `object` changed: patch the live UI rect.
    Object,       ///< `object`'s components/children changed: rebuild that object.
    Structure,    ///< Objects added/removed/reordered/reparented: rebuild the scene.
    Settings,     ///< Only `scene.settings` (config overrides) changed: re-apply them, rebuild nothing.
};

struct Change {
    ChangeScope scope = ChangeScope::None;
    ObjectId object = 0;
};

class SceneDocument {
public:
    // ---------------------------------------------------------------------------------
    // Lifetime
    // ---------------------------------------------------------------------------------

    /**
     * @brief A new, unsaved OBJECT ASSET (objects/<name>.yaml): one root object named `name`.
     *        Object assets are edited as a one-object scene (see is_object_asset()).
     */
    void reset_object(const std::string& name);

    /** @brief A new, empty, unsaved scene named `name`. */
    void reset(const std::string& name = "untitled");

    /** @brief Loads `path` (.yaml or .caml). @throws on a missing or malformed file. */
    void load(const std::filesystem::path& path);

    /** @brief The document as it will be written (private keys stripped). */
    Node clean_copy() const;

    /** @brief An object asset's top-level keys besides `object:` (e.g. the UI designer's `ui_editor:`). */
    const Node& object_extras() const { return object_extras_; }
    /** @brief Sets one of those keys; written by the next save(). Not an undoable edit. */
    void set_object_extra(const std::string& key, Node value);

    /** @brief True when this document is an object asset (objects/*.yaml), not a scene. */
    bool is_object_asset() const { return object_asset_; }
    /** @brief The object asset's root object id (0 for scenes). */
    ObjectId object_root() const;

    /** @brief Saves to `path` (or the current path). @throws on I/O failure. */
    void save(const std::filesystem::path& path = {});

    const Node& node() const { return doc_; }
    const std::filesystem::path& path() const { return path_; }
    void set_path(const std::filesystem::path& p) { path_ = p; }
    bool dirty() const { return undo_.revision() != saved_revision_; }
    /** @brief Bumps with every undoable change (and undo / redo). */
    uint64_t undo_revision() const { return undo_.revision(); }
    std::string scene_name() const { return get_string(doc_.at("scene"), "scene_name", "Scene"); }

    UndoStack<Node>& undo_stack() { return undo_; }

    // ---------------------------------------------------------------------------------
    // Object access
    // ---------------------------------------------------------------------------------

    Node& root_objects() { return doc_["scene"]["root_objects"]; }
    const Node& root_objects() const { return doc_.at("scene").at("root_objects"); }

    /** @brief The object node with editor id `id`, or null. */
    Node* find(ObjectId id);
    const Node* find(ObjectId id) const { return const_cast<SceneDocument*>(this)->find(id); }

    /** @brief The id of an object node (0 if unstamped). */
    static ObjectId id_of(const Node& obj) {
        return obj.is_mapping() && obj.contains(kEidKey) ? obj.at(kEidKey).get_value<int64_t>() : 0;
    }

    /** @brief The parent object's id (0 for a root object), or nullopt if `id` is unknown. */
    std::optional<ObjectId> parent_of(ObjectId id) const;

    /** @brief True if `ancestor` is `id` or one of its ancestors. */
    bool is_ancestor(ObjectId ancestor, ObjectId id) const;

    /** @brief Calls fn(node, depth) for every object, depth-first, in document order. */
    void for_each_object(const std::function<void(const Node&, int)>& fn) const {
        walk_(root_objects(), 0, fn);
    }

    /** @brief Every object id in document order. */
    std::vector<ObjectId> all_ids() const;

    // ---------------------------------------------------------------------------------
    // Selection (not part of undo)
    // ---------------------------------------------------------------------------------

    const std::vector<ObjectId>& selection() const { return selection_; }
    ObjectId primary() const { return selection_.empty() ? 0 : selection_.back(); }
    bool is_selected(ObjectId id) const { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); }
    void select(ObjectId id, bool additive = false);
    void clear_selection() { selection_.clear(); }
    void prune_selection();

    // ---------------------------------------------------------------------------------
    // Edits -- every mutation goes through edit() so it is undoable
    // ---------------------------------------------------------------------------------

    /**
     * @brief Applies `fn` to the document as one undoable step.
     * @param merge_key Merges consecutive edits with the same key (drags); see UndoStack.
     * @return The change's scope, for the live-scene sync.
     */
    Change edit(const std::string& label, const std::function<Change(Node& doc)>& fn, const std::string& merge_key = {});

    /** @brief Closes the undo merge window (end of a drag). */
    void end_merge() { undo_.end_merge(); }

    Change undo();
    Change redo();

    /** @brief A fresh object node (Transform included) with a new id. */
    Node make_object(const std::string& name);

    /**
     * @brief A name unique among the parent's children: "cube", "cube_001", "cube_002"... (the
     *        snake_case numbering assets/ uses). A base that already carries a number
     *        ("cube_001", or Blender's "Cube.001") is renumbered rather than suffixed again.
     */
    std::string unique_name(std::string base, ObjectId parent = 0) const;

    /**
     * @brief Inserts an object (from make_object()) under `parent` (0 = root) at `index`
     *        (-1 = end), as one undoable step. Restamps ids in the subtree if they collide.
     * @return The new object's id.
     */
    ObjectId add_object(Node obj, ObjectId parent = 0, int index = -1, const std::string& label = "Add Object");

    /** @brief Removes objects (and their subtrees) as one step. */
    Change delete_objects(const std::vector<ObjectId>& in_ids);

    /** @brief Duplicates objects next to their originals. @return The copies' ids. */
    std::vector<ObjectId> duplicate_objects(const std::vector<ObjectId>& ids);

    /**
     * @brief Moves `id` under `new_parent` (0 = root) at `index` (-1 = end).
     *        Refuses to parent an object under itself or its descendants.
     */
    Change reparent(ObjectId id, ObjectId new_parent, int index = -1);

    /** @brief Sets an object-level key (name, active). */
    Change set_object_key(ObjectId id, const std::string& key, Node value, const std::string& label);

    /** @brief The component list of an object (empty sequence created if missing). */
    Node* components(ObjectId id);

    /** @brief The index of the first component of `type` on `id`, or -1. */
    int find_component(ObjectId id, const std::string& type) const;

    /**
     * @brief Replaces component `index` of `id` wholesale.
     * @param merge_key See edit().
     */
    Change set_component(ObjectId id, int index, Node comp, const std::string& label, const std::string& merge_key = {});

    Change add_component(ObjectId id, Node comp);

    Change remove_component(ObjectId id, int index);

    /** @brief Moves component `index` one slot up (-1) or down (+1). */
    Change move_component(ObjectId id, int index, int dir);

    /** @brief Sets a `scene:`-level key (scene_name, auto_transform). */
    Change set_scene_key(const std::string& key, Node value);

    /**
     * @brief The scene's config overrides -- `scene.settings`, sections of config.yaml
     *        (render, physics) whose keys win over the project's for this scene. An empty
     *        mapping when there are none or this is not a scene.
     */
    Node scene_settings() const;

    /** @brief One override, or nullptr: `scene.settings.<section>.<key>`. */
    const Node* scene_setting(const std::string& section, const std::string& key) const;

    /**
     * @brief Sets (or, with a null `value`, removes) one override. Empty sections and an empty
     *        `settings` block are dropped, so a scene with no overrides has no `settings:` key.
     *        Undoable; ChangeScope::Settings (nothing rebuilds).
     */
    Change set_scene_setting(const std::string& section, const std::string& key, const Node* value, const std::string& label,
                             const std::string& merge_key = {});

    /** @brief Number of overrides in `scene.settings.<section>`. */
    size_t scene_setting_count(const std::string& section) const;

    /**
     * @brief Drops every override in `scene.settings.<section>` as one undoable change (the
     *        scene goes back to the project's values there). Other sections are kept.
     */
    Change clear_scene_settings(const std::string& section, const std::string& label);

    // ---------------------------------------------------------------------------------
    // Prefab instances
    // ---------------------------------------------------------------------------------

    /** @brief True for an instance root: a node with `prefab:` (or `inherit_from:`). */
    static bool is_instance_node(const Node& n) { return n.is_mapping() && (n.contains("prefab") || n.contains("inherit_from")); }
    bool is_instance(ObjectId id) const;

    /** @brief True for a node standing for a child an instance inherits from its object asset. */
    bool is_inherited(ObjectId id) const;

    /**
     * @brief The instance `id` belongs to: itself when it is an instance root, the nearest
     *        instance above it when it is an inherited child, else 0.
     */
    ObjectId instance_root_of(ObjectId id) const;

    /**
     * @brief The outermost instance `id` is part of (an instance placed under another
     *        instance's inherited child belongs to that one too), or 0.
     */
    ObjectId outermost_instance_of(ObjectId id) const;

    /** @brief Child names from instance root `root` down to `id` (empty for the root itself). */
    std::vector<std::string> path_in_instance(ObjectId root, ObjectId id) const;

    /**
     * @brief Gives every child an instance inherits from its object asset a node of its own --
     *        a placeholder holding just the name and an id, marked kInheritedKey -- so it can be
     *        selected, shown in the Hierarchy and given overrides like any object. SceneInheritance
     *        merges a child override by name, so a placeholder changes nothing; one that still
     *        overrides nothing is dropped on save (strip_private_keys()).
     *
     * Inherited children come first, in the asset's order; children the instance adds follow.
     * A former inherited child the asset no longer has is dropped if empty, else kept as an
     * added child (which is what the runtime makes of it). Not an undoable edit.
     *
     * @param asset_of The object asset an instance node resolves to (its children included),
     *                 or nullopt if it can't be read.
     */
    void sync_placeholders(const std::function<std::optional<Node>(const Node& instance)>& asset_of);

    /** @brief Convenience: reads an object's Transform (position, rotation degrees, scale). */
    bool get_transform(ObjectId id, glm::vec3& pos, glm::vec3& rot, glm::vec3& scl) const;

    /** @brief Writes an object's Transform (adding the component if missing). */
    Change set_transform(ObjectId id, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scl,
                         const std::string& label, const std::string& merge_key = {});

private:
    /** @brief sync_placeholders() for one object against what its asset says it holds. */
    void reconcile_placeholders_(Node& obj, const Node& asset);

    /**
     * @brief id -> object node and id -> parent id, so find()/parent_of()/is_ancestor() don't walk
     *        the tree: the editor calls them per object per frame (outliner rows, overlays), which
     *        made every such pass quadratic in object count.
     *
     * Valid while `gen == generation_`. generation_ is bumped by everything that can move object
     * nodes -- edit() (before and after its callback), undo/redo, load, reset -- and the index is
     * rebuilt lazily by one walk. Inside an edit the tree is being rearranged under the callback,
     * so lookups walk instead (edit_depth_). A copy of the document never inherits the index (its
     * pointers would point into the source's tree).
     */
    struct IdIndex {
        std::unordered_map<ObjectId, Node*> nodes;
        std::unordered_map<ObjectId, ObjectId> parents;
        uint64_t gen = ~0ull;
        IdIndex() = default;
        IdIndex(const IdIndex&) {}
        IdIndex(IdIndex&&) noexcept {}
        IdIndex& operator=(const IdIndex&) { invalidate(); return *this; }
        IdIndex& operator=(IdIndex&&) noexcept { invalidate(); return *this; }
        void invalidate() {
            nodes.clear();
            parents.clear();
            gen = ~0ull;
        }
    };

    void refresh_index_();

    void index_rec_(Node& list, ObjectId parent);

    Node* find_in_(Node& list, ObjectId id);

    bool find_parent_(const Node& list, ObjectId id, ObjectId parent, ObjectId& out) const;

    void walk_(const Node& list, int depth, const std::function<void(const Node&, int)>& fn) const;

    Node* children_list_(ObjectId parent);

    static int index_in_(const Node& list, ObjectId id);

    bool remove_(Node& list, ObjectId id);

    void stamp_ids_(Node& list);

    /** @brief Gives `obj` and its subtree ids unused in this document. */
    void restamp_subtree_(Node& obj);

    /** @brief Every SaveId component's `id` in the document. */
    std::set<std::string> save_ids_() const;

    /** @brief "<name>_<6 hex>" (name sanitized), unused in `used`; recorded there. */
    static std::string unique_save_id_(const std::string& name, std::set<std::string>& used);

    /** @brief Gives every SaveId in `obj`'s subtree a fresh id (a duplicated object is a new one). */
    void fresh_save_ids_(Node& obj, std::set<std::string>& used);

    void restamp_all_(Node& obj);

    Node doc_ = Node::mapping();
    IdIndex index_;
    uint64_t generation_ = 0;
    int edit_depth_ = 0;
    std::filesystem::path path_;
    ObjectId next_eid_ = 1;
    bool object_asset_ = false;
    Node object_extras_ = Node::mapping();   ///< An object asset's other top-level keys (kept for save()).
    UndoStack<Node> undo_;
    uint64_t saved_revision_ = 0;
    std::vector<ObjectId> selection_;
};

} // namespace toy::editor

#endif // TOYEDITOR_CORE_SCENE_DOCUMENT_H
