#include "editor/app/scene_sync.h"

namespace toy {
namespace editor {

coopa::scene::Scene* SceneSync::rebuild(core::Engine& engine, const SceneDocument& doc) {
    live_.clear();
    const std::string path = anchor_(doc);
    install_observer_();
    try {
        coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc.node(), path);
        remove_observer_();
        scene_ = &engine.set_scene(std::move(scene), doc.scene_settings());
    } catch (const std::exception& e) {
        remove_observer_();
        last_error = e.what();
        std::cerr << "[editor] Scene rebuild failed: " << e.what() << "\n";
        coopa::scene::Scene empty("Empty");
        scene_ = &engine.set_scene(std::move(empty));
    }
    return scene_;
}

void SceneSync::apply(core::Engine& engine, const SceneDocument& doc, const Change& c) {
    if (!scene_ || c.scope == ChangeScope::None) return;
    if (c.scope == ChangeScope::Structure || c.object == 0) { rebuild(engine, doc); return; }
    if (c.scope == ChangeScope::Transform && patch_transform_(doc, c.object)) return;
    if (c.scope == ChangeScope::Rect && patch_rect(doc, c.object)) return;
    if (!rebuild_object_(engine, doc, c.object)) rebuild(engine, doc);
}

void SceneSync::forget_scene() { scene_ = nullptr; live_.clear(); }

coopa::scene::SceneObject* SceneSync::live(ObjectId id) const {
    auto it = live_.find(id);
    return it == live_.end() ? nullptr : it->second;
}

ObjectId SceneSync::id_of_live(const coopa::scene::SceneObject* obj) const {
    for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
        for (const auto& [id, ptr] : live_) if (ptr == o) return id;
    }
    return 0;
}

bool SceneSync::patch_rect(const SceneDocument& doc, ObjectId id) {
    coopa::scene::SceneObject* obj = live(id);
    auto* rt = obj ? obj->get_component<coopa::ui::RectTransform>() : nullptr;
    const int ci = doc.find_component(id, "RectTransform");
    if (!rt || ci < 0 || inherits_(doc, id)) return false;
    coopa::ui::RectTransform fresh;
    coopa::ui::detail::parse_rect_transform(doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)], fresh);
    rt->params() = fresh.params();
    rt->set_local_rotation_degrees(fresh.local_rotation_degrees());
    rt->set_local_scale(fresh.local_scale());
    rt->hittable = fresh.hittable;
    rt->z_order = fresh.z_order;
    return true;
}

void SceneSync::install_observer_() {
    coopa::scene::SceneLoader::set_object_observer([this](const fkyaml::node& node, coopa::scene::SceneObject& obj) {
        if (node.is_mapping() && node.contains(kEidKey)) live_[node.at(kEidKey).get_value<int64_t>()] = &obj;
    });
}

bool SceneSync::patch_transform_(const SceneDocument& doc, ObjectId id) {
    coopa::scene::SceneObject* obj = live(id);
    if (!obj || !obj->get_transform()) return false;
    const int ci = doc.find_component(id, "Transform");
    if (ci < 0) return false;
    const Node& tn = doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
    auto& t = obj->get_transform()->transform();
    // An inherited child's Transform is an override: only the keys it holds differ from
    // the object asset's, and the live object already has the asset's for the rest.
    const bool partial = doc.is_inherited(id);
    if (!partial || tn.contains("position")) t.set_position(get_vec3(tn, "position"));
    if (!partial || tn.contains("rotation")) t.set_rotation(get_vec3(tn, "rotation"));
    if (!partial || tn.contains("scale")) t.set_scale(get_vec3(tn, "scale", glm::vec3(1.0f)));
    return true;
}

bool SceneSync::rebuildable_(const SceneDocument& doc, ObjectId id) {
    if (doc.node().at("scene").contains("inherit_from")) return false;
    const Node* self = doc.find(id);
    if (!self || self->contains(kInheritedKey)) return false;
    for (std::optional<ObjectId> cur = doc.parent_of(id); cur && *cur != 0; cur = doc.parent_of(*cur)) {
        const Node* n = doc.find(*cur);
        if (!n || SceneDocument::is_instance_node(*n) || n->contains(kInheritedKey)) return false;
    }
    return true;
}

bool SceneSync::inherits_(const SceneDocument& doc, ObjectId id) {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = doc.parent_of(*cur)) {
        const Node* n = doc.find(*cur);
        if (n && (n->contains("inherit_from") || n->contains("prefab"))) return true;
    }
    return doc.node().at("scene").contains("inherit_from");
}

void SceneSync::forget_subtree_(coopa::scene::SceneObject* obj) {
    for (auto it = live_.begin(); it != live_.end();) {
        bool inside = false;
        for (const coopa::scene::SceneObject* o = it->second; o; o = o->parent()) if (o == obj) { inside = true; break; }
        it = inside ? live_.erase(it) : std::next(it);
    }
}

bool SceneSync::rebuild_object_(core::Engine& engine, const SceneDocument& doc, ObjectId id) {
    coopa::scene::SceneObject* old = live(id);
    const Node* node = doc.find(id);
    if (!old || !node || !rebuildable_(doc, id)) return false;
    const bool auto_transform = get_bool(doc.node().at("scene"), "auto_transform", true);
    coopa::scene::SceneObject* parent = old->parent();
    coopa::scene::TransformComponent* parent_tc = parent ? parent->get_transform() : nullptr;

    install_observer_();
    std::unique_ptr<coopa::scene::SceneObject> fresh;
    try {
        fresh = coopa::scene::SceneLoader::build_object(*node, parent_tc, anchor_(doc), auto_transform);
    } catch (const std::exception& e) {
        remove_observer_();
        last_error = e.what();
        return false;
    }
    remove_observer_();

    engine.wait_idle();
    // The fresh subtree registered itself under the same ids; drop only stale pointers.
    std::unordered_map<ObjectId, coopa::scene::SceneObject*> fresh_ids;
    for (const auto& [k, v] : live_) {
        for (const coopa::scene::SceneObject* o = v; o; o = o->parent()) {
            if (o == fresh.get()) { fresh_ids[k] = v; break; }
        }
    }
    forget_subtree_(old);
    for (const auto& kv : fresh_ids) live_[kv.first] = kv.second;

    coopa::scene::SceneObject* raw = nullptr;
    if (parent) {
        parent->detach_child(old);
        raw = parent->add_child(std::move(fresh));
    } else {
        scene_->remove_root_object(old);
        raw = scene_->add_root_object(std::move(fresh));
    }
    scene_->adopt(*raw);
    raw->start();
    return true;
}

} // namespace editor
} // namespace toy
