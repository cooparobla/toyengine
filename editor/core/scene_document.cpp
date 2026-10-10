#include "editor/core/scene_document.h"

namespace toy {
namespace editor {

void SceneDocument::reset_object(const std::string& name) {
    reset(name);
    object_asset_ = true;
    Node obj = make_object(name);
    root_objects().as_seq().push_back(obj);
    ++generation_;
    undo_.clear();
    saved_revision_ = undo_.revision() - 1;   // unsaved: dirty from the start
}

void SceneDocument::reset(const std::string& name) {
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

void SceneDocument::load(const std::filesystem::path& path) {
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

Node SceneDocument::clean_copy() const {
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

void SceneDocument::set_object_extra(const std::string& key, Node value) {
    if (!object_extras_.is_mapping()) object_extras_ = Node::mapping();
    object_extras_[key] = std::move(value);
}

ObjectId SceneDocument::object_root() const {
    if (!object_asset_) return 0;
    const Node& roots = root_objects();
    return roots.is_sequence() && roots.size() > 0 ? id_of(roots.as_seq()[0]) : 0;
}

void SceneDocument::save(const std::filesystem::path& path) {
    if (!path.empty()) path_ = path;
    if (path_.empty()) throw std::runtime_error("Scene has no file path");
    coopa::yaml::save_document(path_, clean_copy());
    saved_revision_ = undo_.revision();
}

Node* SceneDocument::find(ObjectId id) {
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

std::optional<ObjectId> SceneDocument::parent_of(ObjectId id) const {
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

bool SceneDocument::is_ancestor(ObjectId ancestor, ObjectId id) const {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = parent_of(*cur)) {
        if (*cur == ancestor) return true;
    }
    return false;
}

std::vector<ObjectId> SceneDocument::all_ids() const {
    std::vector<ObjectId> ids;
    for_each_object([&](const Node& n, int) { ids.push_back(id_of(n)); });
    return ids;
}

void SceneDocument::select(ObjectId id, bool additive) {
    if (!additive) selection_.clear();
    if (id == 0) return;
    auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        if (additive) { selection_.erase(it); return; }
    } else {
        selection_.push_back(id);
    }
}

void SceneDocument::prune_selection() {
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [this](ObjectId id) { return find(id) == nullptr; }), selection_.end());
}

Change SceneDocument::edit(const std::string& label, const std::function<Change(Node& doc)>& fn, const std::string& merge_key) {
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

Change SceneDocument::undo() {
    if (const Node* prev = undo_.undo()) { doc_ = *prev; ++generation_; prune_selection(); return {ChangeScope::Structure, 0}; }
    return {};
}

Change SceneDocument::redo() {
    if (const Node* next = undo_.redo()) { doc_ = *next; ++generation_; prune_selection(); return {ChangeScope::Structure, 0}; }
    return {};
}

Node SceneDocument::make_object(const std::string& name) {
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

std::string SceneDocument::unique_name(std::string base, ObjectId parent) const {
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

ObjectId SceneDocument::add_object(Node obj, ObjectId parent, int index, const std::string& label) {
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

Change SceneDocument::delete_objects(const std::vector<ObjectId>& in_ids) {
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

std::vector<ObjectId> SceneDocument::duplicate_objects(const std::vector<ObjectId>& ids) {
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

Change SceneDocument::reparent(ObjectId id, ObjectId new_parent, int index) {
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

Change SceneDocument::set_object_key(ObjectId id, const std::string& key, Node value, const std::string& label) {
    if (key == "name" && is_inherited(id)) return {};   // overrides match inherited children by name
    return edit(label, [&](Node&) -> Change {
        Node* obj = find(id);
        if (!obj) return {};
        (*obj)[key] = value;
        return {key == "active" ? ChangeScope::Object : ChangeScope::Structure, id};
    });
}

Node* SceneDocument::components(ObjectId id) {
    Node* obj = find(id);
    if (!obj) return nullptr;
    return &ensure_seq(*obj, "components");
}

int SceneDocument::find_component(ObjectId id, const std::string& type) const {
    const Node* obj = find(id);
    if (!obj || !obj->contains("components")) return -1;
    const auto& seq = obj->at("components").as_seq();
    for (size_t i = 0; i < seq.size(); ++i) if (component_type(seq[i]) == type) return static_cast<int>(i);
    return -1;
}

Change SceneDocument::set_component(ObjectId id, int index, Node comp, const std::string& label, const std::string& merge_key) {
    return edit(label, [&](Node&) -> Change {
        Node* list = components(id);
        if (!list || index < 0 || index >= static_cast<int>(list->size())) return {};
        const std::string type = component_type(list->as_seq()[static_cast<size_t>(index)]);
        list->as_seq()[static_cast<size_t>(index)] = comp;
        return {type == "Transform" ? ChangeScope::Transform : type == "RectTransform" ? ChangeScope::Rect : ChangeScope::Object, id};
    }, merge_key);
}

Change SceneDocument::add_component(ObjectId id, Node comp) {
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

Change SceneDocument::remove_component(ObjectId id, int index) {
    return edit("Remove Component", [&](Node&) -> Change {
        Node* list = components(id);
        if (!list || index < 0 || index >= static_cast<int>(list->size())) return {};
        list->as_seq().erase(list->as_seq().begin() + index);
        return {ChangeScope::Object, id};
    });
}

Change SceneDocument::move_component(ObjectId id, int index, int dir) {
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

Change SceneDocument::set_scene_key(const std::string& key, Node value) {
    return edit("Scene Settings", [&](Node& d) -> Change {
        d["scene"][key] = value;
        return {ChangeScope::Structure, 0};
    });
}

Node SceneDocument::scene_settings() const {
    if (doc_.is_mapping() && doc_.contains("scene") && doc_.at("scene").is_mapping() &&
        doc_.at("scene").contains("settings") && doc_.at("scene").at("settings").is_mapping()) {
        return doc_.at("scene").at("settings");
    }
    return Node::mapping();
}

const Node* SceneDocument::scene_setting(const std::string& section, const std::string& key) const {
    if (!doc_.is_mapping() || !doc_.contains("scene")) return nullptr;
    const Node& sc = doc_.at("scene");
    if (!sc.is_mapping() || !sc.contains("settings")) return nullptr;
    const Node& st = sc.at("settings");
    if (!st.is_mapping() || !st.contains(section) || !st.at(section).is_mapping()) return nullptr;
    const Node& sec = st.at(section);
    return sec.contains(key) ? &sec.at(key) : nullptr;
}

Change SceneDocument::set_scene_setting(const std::string& section, const std::string& key, const Node* value, const std::string& label,
                         const std::string& merge_key) {
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

size_t SceneDocument::scene_setting_count(const std::string& section) const {
    const Node s = scene_settings();
    return s.contains(section) && s.at(section).is_mapping() ? s.at(section).size() : 0;
}

Change SceneDocument::clear_scene_settings(const std::string& section, const std::string& label) {
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

bool SceneDocument::is_instance(ObjectId id) const { const Node* n = find(id); return n && is_instance_node(*n); }

bool SceneDocument::is_inherited(ObjectId id) const { const Node* n = find(id); return n && n->contains(kInheritedKey); }

ObjectId SceneDocument::instance_root_of(ObjectId id) const {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = parent_of(*cur)) {
        const Node* n = find(*cur);
        if (!n) return 0;
        if (is_instance_node(*n)) return *cur;
        if (!n->contains(kInheritedKey)) return 0;
    }
    return 0;
}

ObjectId SceneDocument::outermost_instance_of(ObjectId id) const {
    ObjectId root = instance_root_of(id);
    while (root) {
        const auto parent = parent_of(root);
        const ObjectId up = parent && *parent ? instance_root_of(*parent) : 0;
        if (!up) break;
        root = up;
    }
    return root;
}

std::vector<std::string> SceneDocument::path_in_instance(ObjectId root, ObjectId id) const {
    std::vector<std::string> path;
    for (std::optional<ObjectId> cur = id; cur && *cur != 0 && *cur != root; cur = parent_of(*cur)) {
        const Node* n = find(*cur);
        if (!n) return {};
        path.insert(path.begin(), get_string(*n, "name"));
    }
    return path;
}

void SceneDocument::sync_placeholders(const std::function<std::optional<Node>(const Node& instance)>& asset_of) {
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

bool SceneDocument::get_transform(ObjectId id, glm::vec3& pos, glm::vec3& rot, glm::vec3& scl) const {
    const int ci = find_component(id, "Transform");
    pos = glm::vec3(0); rot = glm::vec3(0); scl = glm::vec3(1);
    if (ci < 0) return false;
    const Node& t = find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
    pos = get_vec3(t, "position");
    rot = get_vec3(t, "rotation");
    scl = get_vec3(t, "scale", glm::vec3(1.0f));
    return true;
}

Change SceneDocument::set_transform(ObjectId id, const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scl,
                     const std::string& label, const std::string& merge_key) {
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

void SceneDocument::reconcile_placeholders_(Node& obj, const Node& asset) {
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

void SceneDocument::refresh_index_() {
    if (index_.gen == generation_) return;
    index_.nodes.clear();
    index_.parents.clear();
    index_rec_(root_objects(), 0);
    index_.gen = generation_;
}

void SceneDocument::index_rec_(Node& list, ObjectId parent) {
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

Node* SceneDocument::find_in_(Node& list, ObjectId id) {
    if (!list.is_sequence()) return nullptr;
    for (auto& o : list.as_seq()) {
        if (id_of(o) == id) return &o;
        if (o.is_mapping() && o.contains("children")) {
            if (Node* r = find_in_(o["children"], id)) return r;
        }
    }
    return nullptr;
}

bool SceneDocument::find_parent_(const Node& list, ObjectId id, ObjectId parent, ObjectId& out) const {
    if (!list.is_sequence()) return false;
    for (const auto& o : list.as_seq()) {
        if (id_of(o) == id) { out = parent; return true; }
        if (o.is_mapping() && o.contains("children") && find_parent_(o.at("children"), id, id_of(o), out)) return true;
    }
    return false;
}

void SceneDocument::walk_(const Node& list, int depth, const std::function<void(const Node&, int)>& fn) const {
    if (!list.is_sequence()) return;
    for (const auto& o : list.as_seq()) {
        fn(o, depth);
        if (o.is_mapping() && o.contains("children")) walk_(o.at("children"), depth + 1, fn);
    }
}

Node* SceneDocument::children_list_(ObjectId parent) {
    if (parent == 0) return &root_objects();
    Node* p = find(parent);
    if (!p) return nullptr;
    return &ensure_seq(*p, "children");
}

int SceneDocument::index_in_(const Node& list, ObjectId id) {
    const auto& seq = list.as_seq();
    for (size_t i = 0; i < seq.size(); ++i) if (id_of(seq[i]) == id) return static_cast<int>(i);
    return -1;
}

bool SceneDocument::remove_(Node& list, ObjectId id) {
    if (!list.is_sequence()) return false;
    auto& seq = list.as_seq();
    for (size_t i = 0; i < seq.size(); ++i) {
        if (id_of(seq[i]) == id) { seq.erase(seq.begin() + static_cast<long>(i)); return true; }
        if (seq[i].is_mapping() && seq[i].contains("children") && remove_(seq[i]["children"], id)) return true;
    }
    return false;
}

void SceneDocument::stamp_ids_(Node& list) {
    if (!list.is_sequence()) return;
    for (auto& o : list.as_seq()) {
        if (!o.is_mapping()) continue;
        o[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
        if (o.contains("children")) stamp_ids_(o["children"]);
    }
}

void SceneDocument::restamp_subtree_(Node& obj) {
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

std::set<std::string> SceneDocument::save_ids_() const {
    std::set<std::string> used;
    for_each_object([&](const Node& o, int) {
        if (!o.contains("components") || !o.at("components").is_sequence()) return;
        for (const auto& c : o.at("components").as_seq()) {
            if (component_type(c) == "SaveId") used.insert(get_string(c, "id"));
        }
    });
    return used;
}

std::string SceneDocument::unique_save_id_(const std::string& name, std::set<std::string>& used) {
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

void SceneDocument::fresh_save_ids_(Node& obj, std::set<std::string>& used) {
    if (!obj.is_mapping()) return;
    if (obj.contains("components") && obj["components"].is_sequence()) {
        for (auto& c : obj["components"].as_seq()) {
            if (component_type(c) == "SaveId") c["id"] = Node(unique_save_id_(get_string(obj, "name", "object"), used));
        }
    }
    if (obj.contains("children")) for (auto& c : obj["children"].as_seq()) fresh_save_ids_(c, used);
}

void SceneDocument::restamp_all_(Node& obj) {
    if (!obj.is_mapping()) return;
    obj[kEidKey] = Node(static_cast<int64_t>(next_eid_++));
    if (obj.contains("children")) for (auto& c : obj["children"].as_seq()) restamp_all_(c);
}

} // namespace editor
} // namespace toy
