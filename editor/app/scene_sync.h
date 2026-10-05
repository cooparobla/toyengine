/**
 * @file scene_sync.h
 * @brief Keeps the Engine's live scene in step with a SceneDocument, rebuilding as little
 *        as each change needs.
 *
 *   ChangeScope::Transform -> the live object's Transform is patched in place (gizmo drags
 *                             stay at full frame rate);
 *   ChangeScope::Rect      -> likewise its RectTransform (the UI designer's rect gizmo);
 *   ChangeScope::Object    -> that object's subtree is rebuilt from its node;
 *   ChangeScope::Structure -> the whole scene is rebuilt (SceneLoader::load_from_node()).
 *
 * The object observer SceneLoader calls for every object built records `__eid` ->
 * SceneObject*, which is how the viewport maps a click back to a document object.
 */

#ifndef TOYEDITOR_APP_SCENE_SYNC_H
#define TOYEDITOR_APP_SCENE_SYNC_H

#include "../core/scene_document.h"

#include <toyengine/core/engine.h>

#include <coopa/scene/scene_loader.h>

#include <uicoopa/ui_yaml.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

namespace toy::editor {

class SceneSync {
public:
    /** @brief The path load_from_node() anchors relative references at for an unsaved scene. */
    std::filesystem::path fallback_path;

    /** @brief Rebuilds the live scene from scratch; makes it the Engine's (edit) scene. */
    coopa::scene::Scene* rebuild(core::Engine& engine, const SceneDocument& doc) {
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

    /** @brief Applies one document change to the live scene. */
    void apply(core::Engine& engine, const SceneDocument& doc, const Change& c) {
        if (!scene_ || c.scope == ChangeScope::None) return;
        if (c.scope == ChangeScope::Structure || c.object == 0) { rebuild(engine, doc); return; }
        if (c.scope == ChangeScope::Transform && patch_transform_(doc, c.object)) return;
        if (c.scope == ChangeScope::Rect && patch_rect(doc, c.object)) return;
        if (!rebuild_object_(engine, doc, c.object)) rebuild(engine, doc);
    }

    coopa::scene::Scene* scene() const { return scene_; }
    void forget_scene() { scene_ = nullptr; live_.clear(); }

    /** @brief The live object built from document object `id`, or null. */
    coopa::scene::SceneObject* live(ObjectId id) const {
        auto it = live_.find(id);
        return it == live_.end() ? nullptr : it->second;
    }

    /** @brief The document id of a live object (searching up its parents), or 0. */
    ObjectId id_of_live(const coopa::scene::SceneObject* obj) const {
        for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
            for (const auto& [id, ptr] : live_) if (ptr == o) return id;
        }
        return 0;
    }

    const std::unordered_map<ObjectId, coopa::scene::SceneObject*>& live_objects() const { return live_; }

    /**
     * @brief Re-parses the document's RectTransform onto the live one in place. Only for a
     *        plain object: a prefab instance's rect is merged with its asset's, which only a
     *        rebuild resolves.
     */
    bool patch_rect(const SceneDocument& doc, ObjectId id) {
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

    std::string last_error;

private:
    std::string anchor_(const SceneDocument& doc) const {
        return (doc.path().empty() ? fallback_path : doc.path()).string();
    }

    void install_observer_() {
        coopa::scene::SceneLoader::set_object_observer([this](const fkyaml::node& node, coopa::scene::SceneObject& obj) {
            if (node.is_mapping() && node.contains(kEidKey)) live_[node.at(kEidKey).get_value<int64_t>()] = &obj;
        });
    }
    void remove_observer_() { coopa::scene::SceneLoader::set_object_observer({}); }

    bool patch_transform_(const SceneDocument& doc, ObjectId id) {
        coopa::scene::SceneObject* obj = live(id);
        if (!obj || !obj->get_transform()) return false;
        glm::vec3 p, r, s;
        if (!doc.get_transform(id, p, r, s)) return false;
        auto& t = obj->get_transform()->transform();
        t.set_position(p);
        t.set_rotation(r);
        t.set_scale(s);
        return true;
    }

    /** @brief True if `id` or an ancestor uses inherit_from (then only a full rebuild is exact). */
    static bool inherits_(const SceneDocument& doc, ObjectId id) {
        for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = doc.parent_of(*cur)) {
            const Node* n = doc.find(*cur);
            if (n && (n->contains("inherit_from") || n->contains("prefab"))) return true;
        }
        return doc.node().at("scene").contains("inherit_from");
    }

    void forget_subtree_(coopa::scene::SceneObject* obj) {
        for (auto it = live_.begin(); it != live_.end();) {
            bool inside = false;
            for (const coopa::scene::SceneObject* o = it->second; o; o = o->parent()) if (o == obj) { inside = true; break; }
            it = inside ? live_.erase(it) : std::next(it);
        }
    }

    bool rebuild_object_(core::Engine& engine, const SceneDocument& doc, ObjectId id) {
        coopa::scene::SceneObject* old = live(id);
        const Node* node = doc.find(id);
        if (!old || !node || inherits_(doc, id)) return false;
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

    coopa::scene::Scene* scene_ = nullptr;
    std::unordered_map<ObjectId, coopa::scene::SceneObject*> live_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_SCENE_SYNC_H
