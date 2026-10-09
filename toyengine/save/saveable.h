/**
 * @file saveable.h
 * @brief Per-object save state: the SaveId component (a stable identity) and the ISaveable
 *        interface components implement to put their own state in a save.
 *
 * Objects have no runtime ids, so an object that wants to be saved carries a SaveId:
 * @code
 * - type: SaveId
 *   id: player            # unique in the scene; the editor fills in a fresh one when added
 * @endcode
 * When the game saves, every component on a SaveId object that implements ISaveable writes into
 * `objects/<id>/<save_key()>`; when it loads, each one reads its section back (a component with
 * no section in the save is left as the scene file built it):
 * @code
 * class Door : public coopa::scene::Component, public toy::save::ISaveable {
 * public:
 *     bool open = false;
 *     std::string save_key() const override { return "door"; }
 *     void save(toy::save::SaveNode& out) override { out.set("open", open); }
 *     void load(const toy::save::SaveNode& in) override { open = in.get("open", open); }
 *     ...
 * };
 * @endcode
 * An empty `id` falls back to the object's name path ("house:door"), which is stable as long as
 * the hierarchy is. Objects spawned at runtime are not tracked: the game records them in the
 * save's `global` section and respawns them itself (see toyengine/save/README.md).
 */

#ifndef TOYENGINE_SAVE_SAVEABLE_H
#define TOYENGINE_SAVE_SAVEABLE_H

#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/save/save_game.h>

namespace toy {
namespace save {

/**
 * @class ISaveable
 * @brief Implemented by components that keep state in a save. Only components on an object
 *        with a SaveId are asked.
 */
class ISaveable {
public:
    virtual ~ISaveable() = default;
    /** @brief This component's section name under its object (unique per object). */
    virtual std::string save_key() const = 0;
    /** @brief Writes this component's state. */
    virtual void save(SaveNode& out) = 0;
    /** @brief Restores it (called after the scene started, before its next update). */
    virtual void load(const SaveNode& in) = 0;
};

/** @brief "parent:child:object" -- the Scene::find_object_by_path() spelling. */
inline std::string object_path(const coopa::scene::SceneObject& obj) {
    std::string path = obj.name();
    for (const coopa::scene::SceneObject* p = obj.parent(); p; p = p->parent()) path = p->name() + ":" + path;
    return path;
}

/**
 * @class SaveId
 * @brief Gives an object a stable identity in saves. Only marks the object; carries no state.
 */
class SaveId : public coopa::scene::Component {
public:
    std::string id;   ///< Unique per scene; empty uses the object's name path.

    std::string type_name() const override { return "SaveId"; }

    /** @brief The identity used in the save: `id`, else the object's name path. */
    std::string key() const {
        if (!id.empty() || !owner) return id;
        return object_path(*owner);
    }
};

/** @brief The object whose SaveId key is `id` (active or not), or null. */
inline coopa::scene::SceneObject* find_by_save_id(const coopa::scene::Scene& scene, const std::string& id) {
    coopa::scene::SceneObject* found = nullptr;
    for (const auto& root : scene.root_objects()) {
        root->for_each_recursive([&](coopa::scene::SceneObject& obj) {
            if (found) return;
            if (auto* sid = obj.get_component<SaveId>(); sid && sid->key() == id) found = &obj;
        });
        if (found) break;
    }
    return found;
}

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVEABLE_H
