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
    coopa::scene::Scene* rebuild(core::Engine& engine, const SceneDocument& doc);

    /** @brief Applies one document change to the live scene. */
    void apply(core::Engine& engine, const SceneDocument& doc, const Change& c);

    coopa::scene::Scene* scene() const { return scene_; }
    void forget_scene();

    /** @brief The live object built from document object `id`, or null. */
    coopa::scene::SceneObject* live(ObjectId id) const;

    /** @brief The document id of a live object (searching up its parents), or 0. */
    ObjectId id_of_live(const coopa::scene::SceneObject* obj) const;

    const std::unordered_map<ObjectId, coopa::scene::SceneObject*>& live_objects() const { return live_; }

    /**
     * @brief Re-parses the document's RectTransform onto the live one in place. Only for a
     *        plain object: a prefab instance's rect is merged with its asset's, which only a
     *        rebuild resolves.
     */
    bool patch_rect(const SceneDocument& doc, ObjectId id);

    std::string last_error;

private:
    std::string anchor_(const SceneDocument& doc) const {
        return (doc.path().empty() ? fallback_path : doc.path()).string();
    }

    void install_observer_();
    void remove_observer_() { coopa::scene::SceneLoader::set_object_observer({}); }

    bool patch_transform_(const SceneDocument& doc, ObjectId id);

    /**
     * @brief True if `id`'s subtree can be rebuilt on its own: a plain object outside any
     *        instance, or an outermost instance root -- SceneLoader::build_object() resolves an
     *        instance from its node. An inherited child's node holds only overrides, and a node
     *        under an instance is merged into it, so those take their whole instance.
     */
    static bool rebuildable_(const SceneDocument& doc, ObjectId id);

    /** @brief True if `id` or an ancestor uses inherit_from (then only a full rebuild is exact). */
    static bool inherits_(const SceneDocument& doc, ObjectId id);

    void forget_subtree_(coopa::scene::SceneObject* obj);

    bool rebuild_object_(core::Engine& engine, const SceneDocument& doc, ObjectId id);

    coopa::scene::Scene* scene_ = nullptr;
    std::unordered_map<ObjectId, coopa::scene::SceneObject*> live_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_SCENE_SYNC_H
