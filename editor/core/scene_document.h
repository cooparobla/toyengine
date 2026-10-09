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
    void reset_object(const std::string& name) {
        reset(name);
        object_asset_ = true;
        Node obj = make_object(name);
        root_objects().as_seq().push_back(obj);
        ++generation_;
        undo_.clear();
        saved_revision_ = undo_.revision() - 1;   // unsaved: dirty from the start
    }

    /** @brief A new, empty, unsaved scene named `name`. */
    void reset(const std::string& name = "untitled") {
        object_asset_ = false;
        object_extras_ = Node::mapping();
        doc_ = Node::mapping();
        doc_["format"] = Node(std::string("toyengine"));
        Node scene = Node::mapping();
        scene["scene_name"] = Node(name);
        scene["root_objects"] = Node::sequence();
        doc_["scene"] = scene;
        ++generation_;
        path_.clear();
        next_eid_ = 1;
        undo_.clear();
        saved_revision_ = undo_.revision();
        selection_.clear();
    }

    /** @brief Loads `path` (.yaml or .caml). @throws on a missing or malformed file. */
    void load(const std::filesystem::path& path) {
        Node doc = coopa::yaml::load_document(coopa::yaml::resolve_variant(path));
        if (!doc.is_mapping()) throw std::runtime_error("Not a scene document: " + path.string());
        // An object asset (`object:` instead of `scene:`) is edited as a one-object scene so
        // everything scene-shaped (hierarchy, inspector, undo, the live scene) works on it;
        // save() writes it back in its own shape.
        object_asset_ = doc.contains("object") && !doc.contains("scene");
        object_extras_ = Node::mapping();
        if (object_asset_) {
            // Top-level keys besides the object itself ride along to save() unchanged.
            for (const auto& kv : doc.as_map()) {
                const std::string k = kv.first.get_value<std::string>();
                if (k != "object") object_extras_[k] = kv.second;
            }
            Node obj = doc.at("object");
            if (!obj.is_mapping()) obj = Node::mapping();
            if (!obj.contains("name")) obj["name"] = Node(path.stem().string());
            Node wrapped = Node::mapping();
            wrapped["format"] = Node(std::string("toyengine"));
            Node scene = Node::mapping();
            scene["scene_name"] = obj.at("name");
            Node roots = Node::sequence();
            roots.as_seq().push_back(obj);
            scene["root_objects"] = roots;
            wrapped["scene"] = scene;
            doc = std::move(wrapped);
        }
        if (!doc.contains("scene")) doc["scene"] = Node::mapping();
        Node& scene = doc["scene"];
        if (!scene.contains("root_objects") || !scene.at("root_objects").is_sequence()) scene["root_objects"] = Node::sequence();
        doc_ = std::move(doc);
        path_ = path;
        next_eid_ = 1;
        stamp_ids_(doc_["scene"]["root_objects"]);
        ++generation_;
        undo_.clear();
        saved_revision_ = undo_.revision();
        selection_.clear();
    }

    /** @brief The document as it will be written (private keys stripped). */
    Node clean_copy() const {
        Node out = doc_;
        strip_private_keys(out);
        if (!object_asset_) return out;
        Node obj_doc = Node::mapping();
        obj_doc["format"] = Node(std::string("toyengine-object"));
        if (object_extras_.is_mapping()) for (const auto& kv : object_extras_.as_map()) obj_doc[kv.first.get_value<std::string>()] = kv.second;
        const Node& roots = out.at("scene").at("root_objects");
        obj_doc["object"] = roots.is_sequence() && roots.size() > 0 ? roots.as_seq()[0] : Node::mapping();
        return obj_doc;
    }

    /** @brief An object asset's top-level keys besides `object:` (e.g. the UI designer's `ui_editor:`). */
    const Node& object_extras() const { return object_extras_; }
    /** @brief Sets one of those keys; written by the next save(). Not an undoable edit. */
    void set_object_extra(const std::string& key, Node value) {
        if (!object_extras_.is_mapping()) object_extras_ = Node::mapping();
        object_extras_[key] = std::move(value);
    }

    /** @brief True when this document is an object asset (objects/*.yaml), not a scene. */
    bool is_object_asset() const { return object_asset_; }
    /** @brief The object asset's root object id (0 for scenes). */
    ObjectId object_root() const {
        if (!object_asset_) return 0;
        const Node& roots = root_objects();
        return roots.is_sequence() && roots.size() > 0 ? id_of(roots.as_seq()[0]) : 0;
    }

    /** @brief Saves to `path` (or the current path). @throws on I/O failure. */
    void save(const std::filesystem::path& path = {}) {
        if (!path.empty()) path_ = path;
        if (path_.empty()) throw std::runtime_error("Scene has no file path");
        coopa::yaml::save_document(path_, clean_copy());
        saved_revision_ = undo_.revision();
    }

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
    Node* find(ObjectId id) {
        // O(1) through the id index outside an edit; inside one the tree is being rearranged,
        // so walk it (see IdIndex).
        if (edit_depth_ == 0) {
            refresh_index_();
            auto it = index_.nodes.find(id);
            if (it != index_.nodes.end() && id_of(*it->second) == id) return it->second;
            // A miss (or a stale entry) falls through to the walk: rare in the per-frame paths
            // (they look up ids that exist), and a node added behind the index's back is still
            // found -- the next lookup then rebuilds it.
        }
        Node* n = find_in_(root_objects(), id);
        if (n && edit_depth_ == 0) index_.gen = ~0ull;
        return n;
    }
    const Node* find(ObjectId id) const { return const_cast<SceneDocument*>(this)->find(id); }

    /** @brief The id of an object node (0 if unstamped). */
    static ObjectId id_of(const Node& obj) {
        return obj.is_mapping() && obj.contains(kEidKey) ? obj.at(kEidKey).get_value<int64_t>() : 0;
    }

    /** @brief The parent object's id (0 for a root object), or nullopt if `id` is unknown. */
    std::optional<ObjectId> parent_of(ObjectId id) const {
        if (edit_depth_ == 0) {
            auto* self = const_cast<SceneDocument*>(this);
            self->refresh_index_();
            auto it = index_.parents.find(id);
            if (it != index_.parents.end()) return it->second;
        }
        ObjectId parent = 0;
        if (find_parent_(root_objects(), id, 0, parent)) return parent;
        return std::nullopt;
    }

    /** @brief True if `ancestor` is `id` or one of its ancestors. */
    bool is_ancestor(ObjectId ancestor, ObjectId id) const {
        for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = parent_of(*cur)) {
            if (*cur == ancestor) return true;
        }
        return false;
    }

    /** @brief Calls fn(node, depth) for every object, depth-first, in document order. */
    void for_each_object(const std::function<void(const Node&, int)>& fn) const {
        walk_(root_objects(), 0, fn);
    }

    /** @brief Every object id in document order. */
    std::vector<ObjectId> all_ids() const {
        std::vector<ObjectId> ids;
        for_each_object([&](const Node& n, int) { ids.push_back(id_of(n)); });
        return ids;
    }

    // ---------------------------------------------------------------------------------
    // Selection (not part of undo)
    // ---------------------------------------------------------------------------------

    const std::vector<ObjectId>& selection() const { return selection_; }
    ObjectId primary() const { return selection_.empty() ? 0 : selection_.back(); }
    bool is_selected(ObjectId id) const { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); }
    void select(ObjectId id, bool additive = false) {
        if (!additive) selection_.clear();
        if (id == 0) return;
        auto it = std::find(selection_.begin(), selection_.end(), id);
        if (it != selection_.end()) {
            if (additive) { selection_.erase(it); return; }
        } else {
            selection_.push_back(id);
        }
    }
    void clear_selection() { selection_.clear(); }
    void prune_selection() {
        selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                        [this](ObjectId id) { return find(id) == nullptr; }), selection_.end());
    }

    // ---------------------------------------------------------------------------------
    // Edits -- every mutation goes through edit() so it is undoable
    // ---------------------------------------------------------------------------------

    /**
     * @brief Applies `fn` to the document as one undoable step.
     * @param merge_key Merges consecutive edits with the same key (drags); see UndoStack.
     * @return The change's scope, for the live-scene sync.
     */
    Change edit(const std::string& label, const std::function<Change(Node& doc)>& fn, const std::string& merge_key = {}) {
        Node before = doc_;
        ++generation_;
        ++edit_depth_;
        Change c;
        try {
            c = fn(doc_);
        } catch (...) {
            --edit_depth_;
            ++generation_;
            throw;
        }
        --edit_depth_;
        ++generation_;
        if (c.scope == ChangeScope::None) return c;
        undo_.push(label, std::move(before), doc_, merge_key);
        return c;
    }

    /** @brief Closes the undo merge window (end of a drag). */
    void end_merge() { undo_.end_merge(); }

    Change undo() {
        if (const Node* prev = undo_.undo()) { doc_ = *prev; ++generation_; prune_selection(); return {ChangeScope::Structure, 0}; }
        return {};
    }
    Change redo() {
        if (const Node* next = undo_.redo()) { doc_ = *next; ++generation_; prune_selection(); return {ChangeScope::Structure, 0}; }
        return {};
    }

    /** @brief A fresh object node (Transform included) with a new id. */
    Node make_object(const std::string& name) {
        Node obj = Node::mapping();
        obj["name"] = Node(name);
        obj["active"] = Node(true);
        obj[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
        Node comps = Node::sequence();
        Node t = Node::mapping();
        t["type"] = Node(std::string("Transform"));
        t["position"] = make_vec3(glm::vec3(0.0f));
        t["rotation"] = make_vec3(glm::vec3(0.0f));
        t["scale"] = make_vec3(glm::vec3(1.0f));
        comps.as_seq().push_back(t);
        obj["components"] = comps;
        obj["children"] = Node::sequence();
        return obj;
    }

    /**
     * @brief A name unique among the parent's children: "cube", "cube_001", "cube_002"... (the
     *        snake_case numbering assets/ uses). A base that already carries a number
     *        ("cube_001", or Blender's "Cube.001") is renumbered rather than suffixed again.
     */
    std::string unique_name(std::string base, ObjectId parent = 0) const {
        const Node* list = parent == 0 ? &root_objects() : nullptr;
        if (parent != 0) {
            const Node* p = find(parent);
            if (p && p->contains("children")) list = &p->at("children");
        }
        auto taken = [&](const std::string& n) {
            if (!list || !list->is_sequence()) return false;
            for (const auto& o : list->as_seq()) if (get_string(o, "name") == n) return true;
            return false;
        };
        if (!taken(base)) return base;
        // Strip an existing _NNN / .NNN so duplicating cube_001 gives cube_002, not cube_001_001.
        if (base.size() > 4) {
            const size_t k = base.size() - 4;
            const bool digits = std::isdigit(static_cast<unsigned char>(base[k + 1])) && std::isdigit(static_cast<unsigned char>(base[k + 2])) &&
                                std::isdigit(static_cast<unsigned char>(base[k + 3]));
            if (digits && (base[k] == '_' || base[k] == '.')) base.erase(k);
        }
        for (int i = 1; i < 10000; ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "_%03d", i);
            if (!taken(base + buf)) return base + buf;
        }
        return base;
    }

    /**
     * @brief Inserts an object (from make_object()) under `parent` (0 = root) at `index`
     *        (-1 = end), as one undoable step. Restamps ids in the subtree if they collide.
     * @return The new object's id.
     */
    ObjectId add_object(Node obj, ObjectId parent = 0, int index = -1, const std::string& label = "Add Object") {
        if (object_asset_ && parent == 0 && object_root() != 0) parent = object_root();   // one root only
        restamp_subtree_(obj);
        const ObjectId id = id_of(obj);
        edit(label, [&](Node&) -> Change {
            Node* list = children_list_(parent);
            if (!list) return {};
            auto& seq = list->as_seq();
            if (index < 0 || index > static_cast<int>(seq.size())) seq.push_back(obj);
            else seq.insert(seq.begin() + index, obj);
            return {ChangeScope::Structure, id};
        });
        return id;
    }

    /** @brief Removes objects (and their subtrees) as one step. */
    Change delete_objects(const std::vector<ObjectId>& in_ids) {
        std::vector<ObjectId> ids;
        for (ObjectId id : in_ids) if (!(object_asset_ && id == object_root())) ids.push_back(id);   // the asset's root stays
        if (ids.empty()) return {};
        auto c = edit(ids.size() > 1 ? "Delete Objects" : "Delete Object", [&](Node&) -> Change {
            bool any = false;
            for (ObjectId id : ids) any |= remove_(root_objects(), id);
            return any ? Change{ChangeScope::Structure, 0} : Change{};
        });
        prune_selection();
        return c;
    }

    /** @brief Duplicates objects next to their originals. @return The copies' ids. */
    std::vector<ObjectId> duplicate_objects(const std::vector<ObjectId>& ids) {
        std::vector<ObjectId> out;
        edit("Duplicate", [&](Node&) -> Change {
            std::set<std::string> save_ids = save_ids_();   // a copy's SaveIds get fresh ids
            for (ObjectId id : ids) {
                const Node* src = find(id);
                const auto parent = parent_of(id);
                if (!src || !parent) continue;
                Node copy = *src;
                restamp_all_(copy);
                copy["name"] = Node(unique_name(get_string(*src, "name", "Object"), *parent));
                fresh_save_ids_(copy, save_ids);
                Node* list = children_list_(*parent);
                auto& seq = list->as_seq();
                int idx = index_in_(*list, id);
                seq.insert(seq.begin() + (idx + 1), copy);
                out.push_back(id_of(copy));
            }
            return out.empty() ? Change{} : Change{ChangeScope::Structure, 0};
        });
        return out;
    }

    /**
     * @brief Moves `id` under `new_parent` (0 = root) at `index` (-1 = end).
     *        Refuses to parent an object under itself or its descendants.
     */
    Change reparent(ObjectId id, ObjectId new_parent, int index = -1) {
        if (object_asset_) {
            if (id == object_root()) return {};
            if (new_parent == 0) new_parent = object_root();
        }
        if (id == new_parent || (new_parent != 0 && is_ancestor(id, new_parent))) return {};
        if (is_inherited(id)) return {};   // a part of an object asset stays where the asset puts it
        return edit("Reparent", [&](Node&) -> Change {
            const Node* src = find(id);
            if (!src) return {};
            Node moved = *src;
            const auto old_parent = parent_of(id);
            Node* old_list = children_list_(old_parent.value_or(0));
            int old_index = old_list ? index_in_(*old_list, id) : -1;
            remove_(root_objects(), id);
            Node* list = children_list_(new_parent);
            if (!list) return {};
            auto& seq = list->as_seq();
            int at = index;
            if (old_parent && *old_parent == new_parent && old_index >= 0 && at > old_index) --at;
            if (at < 0 || at > static_cast<int>(seq.size())) seq.push_back(moved);
            else seq.insert(seq.begin() + at, moved);
            return {ChangeScope::Structure, id};
        });
    }

    /** @brief Sets an object-level key (name, active). */
    Change set_object_key(ObjectId id, const std::string& key, Node value, const std::string& label) {
        if (key == "name" && is_inherited(id)) return {};   // overrides match inherited children by name
        return edit(label, [&](Node&) -> Change {
            Node* obj = find(id);
            if (!obj) return {};
            (*obj)[key] = value;
            return {key == "active" ? ChangeScope::Object : ChangeScope::Structure, id};
        });
    }

    /** @brief The component list of an object (empty sequence created if missing). */
    Node* components(ObjectId id) {
        Node* obj = find(id);
        if (!obj) return nullptr;
        return &ensure_seq(*obj, "components");
    }

    /** @brief The index of the first component of `type` on `id`, or -1. */
    int find_component(ObjectId id, const std::string& type) const {
        const Node* obj = find(id);
        if (!obj || !obj->contains("components")) return -1;
        const auto& seq = obj->at("components").as_seq();
        for (size_t i = 0; i < seq.size(); ++i) if (component_type(seq[i]) == type) return static_cast<int>(i);
        return -1;
    }

    /**
     * @brief Replaces component `index` of `id` wholesale.
     * @param merge_key See edit().
     */
    Change set_component(ObjectId id, int index, Node comp, const std::string& label, const std::string& merge_key = {}) {
        return edit(label, [&](Node&) -> Change {
            Node* list = components(id);
            if (!list || index < 0 || index >= static_cast<int>(list->size())) return {};
            const std::string type = component_type(list->as_seq()[static_cast<size_t>(index)]);
            list->as_seq()[static_cast<size_t>(index)] = comp;
            return {type == "Transform" ? ChangeScope::Transform : type == "RectTransform" ? ChangeScope::Rect : ChangeScope::Object, id};
        }, merge_key);
    }

    Change add_component(ObjectId id, Node comp) {
        const std::string type = component_type(comp);
        return edit("Add " + type, [&](Node&) -> Change {
            Node* list = components(id);
            if (!list) return {};
            if (type == "SaveId") {   // a new SaveId gets an id no other object uses
                std::set<std::string> used = save_ids_();
                const std::string cur = get_string(comp, "id");
                if (cur.empty() || used.count(cur)) {
                    const Node* obj = find(id);
                    comp["id"] = Node(unique_save_id_(obj ? get_string(*obj, "name", "object") : "object", used));
                }
            }
            list->as_seq().push_back(comp);
            return {ChangeScope::Object, id};
        });
    }

    Change remove_component(ObjectId id, int index) {
        return edit("Remove Component", [&](Node&) -> Change {
            Node* list = components(id);
            if (!list || index < 0 || index >= static_cast<int>(list->size())) return {};
            list->as_seq().erase(list->as_seq().begin() + index);
            return {ChangeScope::Object, id};
        });
    }

    /** @brief Moves component `index` one slot up (-1) or down (+1). */
    Change move_component(ObjectId id, int index, int dir) {
        return edit("Reorder Component", [&](Node&) -> Change {
            Node* list = components(id);
            if (!list) return {};
            auto& seq = list->as_seq();
            const int to = index + dir;
            if (index < 0 || to < 0 || index >= static_cast<int>(seq.size()) || to >= static_cast<int>(seq.size())) return {};
            std::swap(seq[static_cast<size_t>(index)], seq[static_cast<size_t>(to)]);
            return {ChangeScope::Object, id};
        });
    }

    /** @brief Sets a `scene:`-level key (scene_name, auto_transform). */
    Change set_scene_key(const std::string& key, Node value) {
        return edit("Scene Settings", [&](Node& d) -> Change {
            d["scene"][key] = value;
            return {ChangeScope::Structure, 0};
        });
    }

    /**
     * @brief The scene's config overrides -- `scene.settings`, sections of config.yaml
     *        (render, physics) whose keys win over the project's for this scene. An empty
     *        mapping when there are none or this is not a scene.
     */
    Node scene_settings() const {
        if (doc_.is_mapping() && doc_.contains("scene") && doc_.at("scene").is_mapping() &&
            doc_.at("scene").contains("settings") && doc_.at("scene").at("settings").is_mapping()) {
            return doc_.at("scene").at("settings");
        }
        return Node::mapping();
    }

    /** @brief One override, or nullptr: `scene.settings.<section>.<key>`. */
    const Node* scene_setting(const std::string& section, const std::string& key) const {
        if (!doc_.is_mapping() || !doc_.contains("scene")) return nullptr;
        const Node& sc = doc_.at("scene");
        if (!sc.is_mapping() || !sc.contains("settings")) return nullptr;
        const Node& st = sc.at("settings");
        if (!st.is_mapping() || !st.contains(section) || !st.at(section).is_mapping()) return nullptr;
        const Node& sec = st.at(section);
        return sec.contains(key) ? &sec.at(key) : nullptr;
    }

    /**
     * @brief Sets (or, with a null `value`, removes) one override. Empty sections and an empty
     *        `settings` block are dropped, so a scene with no overrides has no `settings:` key.
     *        Undoable; ChangeScope::Settings (nothing rebuilds).
     */
    Change set_scene_setting(const std::string& section, const std::string& key, const Node* value, const std::string& label,
                             const std::string& merge_key = {}) {
        if (is_object_asset()) return {};
        return edit(label, [&](Node& d) -> Change {
            Node& sc = d["scene"];
            Node settings = sc.contains("settings") && sc.at("settings").is_mapping() ? sc.at("settings") : Node::mapping();
            Node sec = settings.contains(section) && settings.at(section).is_mapping() ? settings.at(section) : Node::mapping();
            if (value) sec[key] = *value;
            else erase_key(sec, key);
            if (sec.size() > 0) settings[section] = sec;
            else erase_key(settings, section);
            if (settings.size() > 0) sc["settings"] = settings;
            else erase_key(sc, "settings");
            return {ChangeScope::Settings, 0};
        }, merge_key);
    }

    /** @brief Number of overrides in `scene.settings.<section>`. */
    size_t scene_setting_count(const std::string& section) const {
        const Node s = scene_settings();
        return s.contains(section) && s.at(section).is_mapping() ? s.at(section).size() : 0;
    }

    /**
     * @brief Drops every override in `scene.settings.<section>` as one undoable change (the
     *        scene goes back to the project's values there). Other sections are kept.
     */
    Change clear_scene_settings(const std::string& section, const std::string& label) {
        if (is_object_asset() || scene_setting_count(section) == 0) return {};
        return edit(label, [&](Node& d) -> Change {
            Node& sc = d["scene"];
            Node settings = sc.at("settings");
            erase_key(settings, section);
            if (settings.size() > 0) sc["settings"] = settings;
            else erase_key(sc, "settings");
            return {ChangeScope::Settings, 0};
        });
    }

    // ---------------------------------------------------------------------------------
    // Prefab instances
    // ---------------------------------------------------------------------------------

    /** @brief True for an instance root: a node with `prefab:` (or `inherit_from:`). */
    static bool is_instance_node(const Node& n) { return n.is_mapping() && (n.contains("prefab") || n.contains("inherit_from")); }
    bool is_instance(ObjectId id) const { const Node* n = find(id); return n && is_instance_node(*n); }

    /** @brief True for a node standing for a child an instance inherits from its object asset. */
    bool is_inherited(ObjectId id) const { const Node* n = find(id); return n && n->contains(kInheritedKey); }

    /**
     * @brief The instance `id` belongs to: itself when it is an instance root, the nearest
     *        instance above it when it is an inherited child, else 0.
     */
    ObjectId instance_root_of(ObjectId id) const {
        for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = parent_of(*cur)) {
            const Node* n = find(*cur);
            if (!n) return 0;
            if (is_instance_node(*n)) return *cur;
            if (!n->contains(kInheritedKey)) return 0;
        }
        return 0;
    }

    /**
     * @brief The outermost instance `id` is part of (an instance placed under another
     *        instance's inherited child belongs to that one too), or 0.
     */
    ObjectId outermost_instance_of(ObjectId id) const {
        ObjectId root = instance_root_of(id);
        while (root) {
            const auto parent = parent_of(root);
            const ObjectId up = parent && *parent ? instance_root_of(*parent) : 0;
            if (!up) break;
            root = up;
        }
        return root;
    }

    /** @brief Child names from instance root `root` down to `id` (empty for the root itself). */
    std::vector<std::string> path_in_instance(ObjectId root, ObjectId id) const {
        std::vector<std::string> path;
        for (std::optional<ObjectId> cur = id; cur && *cur != 0 && *cur != root; cur = parent_of(*cur)) {
            const Node* n = find(*cur);
            if (!n) return {};
            path.insert(path.begin(), get_string(*n, "name"));
        }
        return path;
    }

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
    void sync_placeholders(const std::function<std::optional<Node>(const Node& instance)>& asset_of) {
        std::function<void(Node&)> walk = [&](Node& list) {
            if (!list.is_sequence()) return;
            for (auto& o : list.as_seq()) {
                if (!o.is_mapping()) continue;
                if (is_instance_node(o)) {
                    if (auto asset = asset_of(o)) reconcile_placeholders_(o, *asset);
                }
                if (o.contains("children")) walk(o["children"]);
            }
        };
        walk(root_objects());
        ++generation_;   // rewrote instance children outside edit(): the id index is stale
    }

    /** @brief Convenience: reads an object's Transform (position, rotation degrees, scale). */
    bool get_transform(ObjectId id, glm::vec3& pos, glm::vec3& rot, glm::vec3& scl) const {
        const int ci = find_component(id, "Transform");
        pos = glm::vec3(0); rot = glm::vec3(0); scl = glm::vec3(1);
        if (ci < 0) return false;
        const Node& t = find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
        pos = get_vec3(t, "position");
        rot = get_vec3(t, "rotation");
        scl = get_vec3(t, "scale", glm::vec3(1.0f));
        return true;
    }

    /** @brief Writes an object's Transform (adding the component if missing). */
    Change set_transform(ObjectId id, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scl,
                         const std::string& label, const std::string& merge_key = {}) {
        return edit(label, [&](Node&) -> Change {
            Node* list = components(id);
            if (!list) return {};
            int ci = find_component(id, "Transform");
            if (ci < 0) {
                Node t = Node::mapping();
                t["type"] = Node(std::string("Transform"));
                list->as_seq().insert(list->as_seq().begin(), t);
                ci = 0;
            }
            Node& t = list->as_seq()[static_cast<size_t>(ci)];
            t["position"] = make_vec3(pos);
            t["rotation"] = make_vec3(rot);
            t["scale"] = make_vec3(scl);
            return {ChangeScope::Transform, id};
        }, merge_key);
    }

private:
    /** @brief sync_placeholders() for one object against what its asset says it holds. */
    void reconcile_placeholders_(Node& obj, const Node& asset) {
        std::vector<const Node*> asset_kids;
        if (asset.contains("children") && asset.at("children").is_sequence()) {
            std::map<std::string, int> count;
            for (const auto& c : asset.at("children").as_seq()) ++count[get_string(c, "name")];
            // Overrides match by name: an unnamed or twice-used name can't be told apart.
            for (const auto& c : asset.at("children").as_seq()) {
                const std::string n = get_string(c, "name");
                if (!n.empty() && count[n] == 1) asset_kids.push_back(&c);
            }
        }
        const bool had = obj.contains("children") && obj.at("children").is_sequence();
        if (asset_kids.empty() && !had) return;
        std::vector<Node> old = had ? obj.at("children").as_seq() : std::vector<Node>{};
        std::vector<bool> used(old.size(), false);
        Node out = Node::sequence();
        for (const Node* ac : asset_kids) {
            const std::string n = get_string(*ac, "name");
            Node child;
            for (size_t i = 0; i < old.size(); ++i) {
                if (!used[i] && old[i].is_mapping() && get_string(old[i], "name") == n) { child = old[i]; used[i] = true; break; }
            }
            if (!child.is_mapping()) {
                child = Node::mapping();
                child["name"] = Node(n);
                child[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
            }
            child[kInheritedKey] = Node(true);
            // Every document object has both lists (much of the editor reads them); empty
            // ones merge as nothing and are dropped on save.
            ensure_seq(child, "components");
            ensure_seq(child, "children");
            if (!is_instance_node(child)) reconcile_placeholders_(child, *ac);
            out.as_seq().push_back(child);
        }
        for (size_t i = 0; i < old.size(); ++i) {
            if (used[i]) continue;
            Node child = old[i];
            if (child.is_mapping() && child.contains(kInheritedKey)) {
                if (is_empty_placeholder(child)) continue;   // the asset dropped (or renamed) it
                erase_key(child, kInheritedKey);             // an orphaned override: an added child now
            }
            out.as_seq().push_back(child);
        }
        obj["children"] = out;
    }

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

    void refresh_index_() {
        if (index_.gen == generation_) return;
        index_.nodes.clear();
        index_.parents.clear();
        index_rec_(root_objects(), 0);
        index_.gen = generation_;
    }

    void index_rec_(Node& list, ObjectId parent) {
        if (!list.is_sequence()) return;
        for (auto& o : list.as_seq()) {
            const ObjectId id = id_of(o);
            if (id != 0) {
                index_.nodes[id] = &o;
                index_.parents[id] = parent;
            }
            if (o.is_mapping() && o.contains("children")) index_rec_(o["children"], id);
        }
    }

    Node* find_in_(Node& list, ObjectId id) {
        if (!list.is_sequence()) return nullptr;
        for (auto& o : list.as_seq()) {
            if (id_of(o) == id) return &o;
            if (o.is_mapping() && o.contains("children")) {
                if (Node* r = find_in_(o["children"], id)) return r;
            }
        }
        return nullptr;
    }

    bool find_parent_(const Node& list, ObjectId id, ObjectId parent, ObjectId& out) const {
        if (!list.is_sequence()) return false;
        for (const auto& o : list.as_seq()) {
            if (id_of(o) == id) { out = parent; return true; }
            if (o.is_mapping() && o.contains("children") && find_parent_(o.at("children"), id, id_of(o), out)) return true;
        }
        return false;
    }

    void walk_(const Node& list, int depth, const std::function<void(const Node&, int)>& fn) const {
        if (!list.is_sequence()) return;
        for (const auto& o : list.as_seq()) {
            fn(o, depth);
            if (o.is_mapping() && o.contains("children")) walk_(o.at("children"), depth + 1, fn);
        }
    }

    Node* children_list_(ObjectId parent) {
        if (parent == 0) return &root_objects();
        Node* p = find(parent);
        if (!p) return nullptr;
        return &ensure_seq(*p, "children");
    }

    static int index_in_(const Node& list, ObjectId id) {
        const auto& seq = list.as_seq();
        for (size_t i = 0; i < seq.size(); ++i) if (id_of(seq[i]) == id) return static_cast<int>(i);
        return -1;
    }

    bool remove_(Node& list, ObjectId id) {
        if (!list.is_sequence()) return false;
        auto& seq = list.as_seq();
        for (size_t i = 0; i < seq.size(); ++i) {
            if (id_of(seq[i]) == id) { seq.erase(seq.begin() + static_cast<long>(i)); return true; }
            if (seq[i].is_mapping() && seq[i].contains("children") && remove_(seq[i]["children"], id)) return true;
        }
        return false;
    }

    void stamp_ids_(Node& list) {
        if (!list.is_sequence()) return;
        for (auto& o : list.as_seq()) {
            if (!o.is_mapping()) continue;
            o[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
            if (o.contains("children")) stamp_ids_(o["children"]);
        }
    }

    /** @brief Gives `obj` and its subtree ids unused in this document. */
    void restamp_subtree_(Node& obj) {
        std::set<ObjectId> used;
        for (ObjectId id : all_ids()) used.insert(id);
        std::function<void(Node&)> fix = [&](Node& o) {
            if (!o.is_mapping()) return;
            if (!o.contains(kEidKey) || used.count(id_of(o))) o[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
            used.insert(id_of(o));
            next_eid_ = std::max<ObjectId>(next_eid_, id_of(o) + 1);
            if (o.contains("children")) for (auto& c : o["children"].as_seq()) fix(c);
        };
        fix(obj);
    }

    /** @brief Every SaveId component's `id` in the document. */
    std::set<std::string> save_ids_() const {
        std::set<std::string> used;
        for_each_object([&](const Node& o, int) {
            if (!o.contains("components") || !o.at("components").is_sequence()) return;
            for (const auto& c : o.at("components").as_seq()) {
                if (component_type(c) == "SaveId") used.insert(get_string(c, "id"));
            }
        });
        return used;
    }

    /** @brief "<name>_<6 hex>" (name sanitized), unused in `used`; recorded there. */
    static std::string unique_save_id_(const std::string& name, std::set<std::string>& used) {
        std::string base;
        for (char ch : name) {
            const unsigned char u = static_cast<unsigned char>(ch);
            base += std::isalnum(u) ? static_cast<char>(std::tolower(u)) : '_';
        }
        if (base.empty()) base = "object";
        static uint64_t counter = 0;
        std::string id;
        do {
            uint64_t h = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (++counter * 0x9E3779B97F4A7C15ull);
            h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%06x", static_cast<unsigned>(h & 0xFFFFFFu));
            id = base + "_" + hex;
        } while (used.count(id));
        used.insert(id);
        return id;
    }

    /** @brief Gives every SaveId in `obj`'s subtree a fresh id (a duplicated object is a new one). */
    void fresh_save_ids_(Node& obj, std::set<std::string>& used) {
        if (!obj.is_mapping()) return;
        if (obj.contains("components") && obj["components"].is_sequence()) {
            for (auto& c : obj["components"].as_seq()) {
                if (component_type(c) == "SaveId") c["id"] = Node(unique_save_id_(get_string(obj, "name", "object"), used));
            }
        }
        if (obj.contains("children")) for (auto& c : obj["children"].as_seq()) fresh_save_ids_(c, used);
    }

    void restamp_all_(Node& obj) {
        if (!obj.is_mapping()) return;
        obj[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
        if (obj.contains("children")) for (auto& c : obj["children"].as_seq()) restamp_all_(c);
    }

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
