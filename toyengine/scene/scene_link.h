/**
 * @file scene_link.h
 * @brief SceneLink: a trigger volume that loads another scene when the player walks into it.
 *
 * Put one on an object (a doorway, the end of a corridor) and the level change needs no code:
 *
 * @code
 * - type: SceneLink
 *   target_scene: heavy.yaml           # next to this scene's file, an asset path, or a name
 *   transition: loading_screen         # fade | loading_screen | none
 *   loading_screen: ui/loading_screen  # the UI asset shown while it loads
 *   spawn_point: from_hub              # object in the target scene the player is moved to
 *   size: { x: 2, y: 2, z: 3 }         # the trigger box, in the object's local space
 * @endcode
 *
 * The volume is a box of `size` centred on the object (scaled and rotated with it). It fires
 * when a CharacterController's capsule enters it -- tested with a physics overlap query each
 * frame rather than a trigger collider, because kinematic bodies (a CharacterController's) and
 * static triggers never form a contact pair. It fires on entry only: a player that starts the
 * scene inside a link (arriving through the matching door) has to leave it first.
 *
 * Firing posts a SceneLinkRequest to SceneLinkRequests; the Engine takes it next frame and calls
 * load_scene_async() (see Engine::consume_scene_link_requests_()). An engine embedded in a tool
 * (the editor's play mode) ignores them.
 */

#ifndef TOYENGINE_SCENE_SCENE_LINK_H
#define TOYENGINE_SCENE_SCENE_LINK_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <mutex>
#include <string>
#include <vector>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <physxcoopa/system/physics_system.h>

#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

/** @brief One scene change a SceneLink asked for. Strings as authored; the engine resolves them. */
struct SceneLinkRequest {
    std::string target_scene;
    std::string transition = "fade";
    std::string loading_screen = "ui/loading_screen";
    std::string spawn_point;
    glm::vec3   color{0.0f};
    float       fade_time = 0.35f;
    float       min_display_time = 0.0f;
    /// The scene the link fired in; the engine ignores a request from a scene it does not run.
    const coopa::scene::Scene* origin = nullptr;
};

/**
 * @class SceneLinkRequests
 * @brief The process-wide queue SceneLinks post to and the Engine drains each frame.
 */
class SceneLinkRequests {
public:
    static SceneLinkRequests& instance() {
        static SceneLinkRequests q;
        return q;
    }
    void push(SceneLinkRequest r) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(r));
    }
    std::vector<SceneLinkRequest> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SceneLinkRequest> out;
        out.swap(queue_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<SceneLinkRequest> queue_;
};

/**
 * @class SceneLink
 * @brief Loads `target_scene` when a CharacterController enters the box around this object.
 */
class SceneLink : public coopa::scene::Component {
public:
    std::string target_scene;
    std::string transition = "fade";             ///< fade | loading_screen | none
    std::string loading_screen = "ui/loading_screen";
    std::string spawn_point;
    glm::vec3   color{0.0f};
    float       fade_time = 0.35f;
    float       min_display_time = 0.0f;
    glm::vec3   size{2.0f, 2.0f, 2.0f};          ///< Trigger box, local space.

    std::string type_name() const override { return "SceneLink"; }

    /** @brief True once it fired (a link fires at most once per scene run). */
    bool fired() const { return fired_; }

    /** @brief Fires now, as if the player had walked in (scripted level changes, tests). */
    void trigger() {
        if (fired_ || target_scene.empty()) return;
        fired_ = true;
        SceneLinkRequest r;
        r.target_scene = target_scene;
        r.transition = transition;
        r.loading_screen = loading_screen;
        r.spawn_point = spawn_point;
        r.color = color;
        r.fade_time = fade_time;
        r.min_display_time = min_display_time;
        r.origin = scene;
        SceneLinkRequests::instance().push(std::move(r));
    }

    void update(float) override {
        if (fired_ || target_scene.empty() || !scene || !owner) return;
        const bool inside = character_inside_();
        // The first look only records where the player is: arriving inside a link is not entering it.
        if (armed_ && inside && !was_inside_) trigger();
        was_inside_ = inside;
        armed_ = true;
    }

private:
    bool fired_ = false;
    bool armed_ = false;
    bool was_inside_ = false;

    bool character_inside_() const {
        auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
        auto* tc = owner->get_transform();
        if (!physics || !tc) return false;
        const glm::mat4 world = tc->get_world_matrix();
        const glm::vec3 axis_x(world[0]), axis_y(world[1]), axis_z(world[2]);
        const glm::vec3 scale(glm::length(axis_x), glm::length(axis_y), glm::length(axis_z));
        if (scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f) return false;
        coopa::physx::geometry::OBB box;
        box.center = glm::vec3(world[3]);
        box.half_extents = glm::abs(size) * scale * 0.5f;
        box.orientation = glm::quat_cast(glm::mat3(axis_x / scale.x, axis_y / scale.y, axis_z / scale.z));
        for (auto* col : physics->overlap_box(box, ~0u, /*include_triggers=*/false)) {
            if (col && col->owner && col->owner->get_component<CharacterController>()) return true;
        }
        return false;
    }
};

}  // namespace scene
}  // namespace toy

#endif  // TOYENGINE_SCENE_SCENE_LINK_H
