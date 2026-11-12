/**
 * @file transform_component.h
 * @brief Component wrapping coopa::util::Transform for scene-graph hierarchy.
 *
 * Every SceneObject has exactly one TransformComponent as its first component.
 * The component links its coopa::util::Transform to its parent object's Transform
 * to enable hierarchical world-matrix propagation.
 */

#ifndef COOPA_SCENE_COMPONENTS_TRANSFORM_COMPONENT_H
#define COOPA_SCENE_COMPONENTS_TRANSFORM_COMPONENT_H

#include <coopa/scene/component.h>
#include <coopa/util/transform.h>
#include <glm/glm.hpp>
#include <string>

namespace coopa {
namespace scene {

/**
 * @class TransformComponent
 * @brief Wraps coopa::util::Transform, linking it into the SceneObject hierarchy.
 *
 * On update(), checks whether the parent's transform has been recomputed and
 * triggers a recompute of this transform's world matrix if needed.
 *
 * Usage:
 * @code
 * auto* t = obj->get_component<TransformComponent>();
 * t->transform().set_position({1.0f, 0.0f, 0.0f});
 * glm::mat4 world = t->get_world_matrix();
 * @endcode
 */
class TransformComponent : public Component {
public:
    TransformComponent() = default;

    std::string type_name() const override { return "Transform"; }

    /**
     * @brief Returns a reference to the underlying coopa::util::Transform.
     */
    coopa::util::Transform& transform() { return transform_; }

    /**
     * @brief Returns a const reference to the underlying coopa::util::Transform.
     */
    const coopa::util::Transform& transform() const { return transform_; }

    /**
     * @brief Returns the world-space transform matrix.
     *
     * Lazily recomputed from the parent chain when dirty.
     *
     * @return World matrix as glm::mat4.
     */
    glm::mat4 get_world_matrix() const {
        return transform_.get_world_matrix();
    }

    /**
     * @brief Links this transform's parent pointer to another TransformComponent.
     *
     * Should be called by SceneLoader after building the full hierarchy.
     *
     * @param parent_transform The parent TransformComponent's underlying Transform.
     */
    void set_parent_transform(coopa::util::Transform* parent_transform) {
        transform_.set_parent(parent_transform);
        if (parent_transform) {
            parent_transform->add_child(&transform_);
        }
    }

    /**
     * @brief Called every frame; triggers recompute if dirty.
     */
    void update(float /*delta_time*/) override {
        if (transform_.is_dirty()) {
            transform_.recompute();
        }
    }

private:
    coopa::util::Transform transform_; /**< The underlying transform data. */
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_COMPONENTS_TRANSFORM_COMPONENT_H
