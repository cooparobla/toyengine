/**
 * @file transform.h
 * @brief Hierarchical 3D transform with dirty-flag propagation and lazy world-matrix recomputation.
 *
 * Provides position, rotation (Euler degrees), and scale with a parent pointer for
 * scene-graph hierarchy. The world matrix is recomputed lazily and propagated to
 * registered children via dirty flags.
 */

#ifndef COOPA_UTIL_TRANSFORM_H
#define COOPA_UTIL_TRANSFORM_H

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cassert>
#include <vector>
#include <atomic>

namespace coopa {
namespace util {

/**
 * @class Transform
 * @brief Hierarchical 3D transform storing position, Euler rotation (degrees), and scale.
 *
 * The local matrix is composed from position, rotation, and scale.
 * The world matrix multiplies the parent's world matrix by the local matrix.
 * A dirty flag is set automatically when any local property changes, and propagated
 * to all registered children so their world matrices are recomputed on next access.
 *
 * Thread safety: get_world_matrix()/get_local_matrix() lazily mutate the
 * matrix cache from a const method and are therefore NOT safe to call
 * concurrently across threads on transforms that might share a parent chain
 * -- two threads reading anywhere in the same subtree could both write the
 * same cache. coopa::scene::TransformSystem (see
 * coopa/scene/systems/transform_system.h) resolves every transform in a
 * scene, top-down, exactly once per frame; after it has run, world_matrix()
 * is a pure read, safe for any number of concurrent readers (e.g. every
 * worker job in a render-list build, or multiple scenes' renderers running
 * concurrently). Structural edits (set_parent()/add_child()/remove_child())
 * and property setters (set_position() etc.) remain owner-thread-only, same
 * as the rest of a Scene -- see Scene's class doc on single-owner + deferred
 * commands.
 */
class Transform {
public:
    /**
     * @brief Default constructs an identity transform (origin, no rotation, unit scale).
     */
    Transform()
        : position_(0.0f), rotation_degrees_(0.0f), scale_(1.0f),
          parent_(nullptr), dirty_(true)
    {}

    // --- Local property setters (each marks dirty and propagates) ---

    /**
     * @brief Sets the local position.
     * @param p New position.
     */
    void set_position(const glm::vec3& p) {
        position_ = p;
        mark_dirty();
    }

    /**
     * @brief Sets the local rotation in Euler degrees (XYZ order).
     * @param r Euler angles in degrees.
     */
    void set_rotation(const glm::vec3& r) {
        rotation_degrees_ = r;
        use_quat_ = false;
        mark_dirty();
    }

    /**
     * @brief Sets the local rotation from a quaternion, for callers (e.g. physxcoopa) that
     *        integrate orientation directly and would otherwise lose precision converting
     *        through Euler degrees every frame.
     *
     * Also updates rotation_degrees_ (via the exact analytic inverse of the ZYX composition
     * recompute_() uses) so rotation_degrees()/set_rotation() keep behaving identically for
     * every other reader -- there is exactly one rotation, expressed two ways, never two
     * that can silently disagree. Without this, an object animated via AnimatedProperty's
     * "rotation" binding (which reads rotation_degrees() and writes set_rotation(vec3), see
     * coopa/animation/animated_property.h) and also driven by a quaternion writer on the
     * same frame would have the two fight over which representation is current.
     *
     * @param q New orientation.
     */
    void set_rotation_quat(const glm::quat& q) {
        rotation_quat_ = q;
        glm::vec3 euler_radians;
        glm::extractEulerAngleZYX(glm::mat4_cast(q), euler_radians.z, euler_radians.y, euler_radians.x);
        rotation_degrees_ = glm::degrees(euler_radians);
        use_quat_ = true;
        mark_dirty();
    }

    /**
     * @brief Sets the local scale.
     * @param s Scale factors.
     */
    void set_scale(const glm::vec3& s) {
        scale_ = s;
        mark_dirty();
    }

    // --- Accessors ---

    /** @brief Returns local position. */
    const glm::vec3& position() const { return position_; }
    /** @brief Returns local rotation in Euler degrees. */
    const glm::vec3& rotation_degrees() const { return rotation_degrees_; }

    /**
     * @brief Returns the local rotation as a quaternion.
     *
     * If the rotation was last set via set_rotation_quat(), returns that value exactly (no
     * round-trip through Euler degrees). Otherwise derives it from rotation_degrees_ on
     * demand -- cheap, and avoids paying the conversion for every Transform that never uses
     * the quaternion path.
     */
    const glm::quat& rotation_quat() const {
        if (!use_quat_) {
            rotation_quat_ = glm::quat(glm::radians(rotation_degrees_));
        }
        return rotation_quat_;
    }
    /** @brief Returns local scale. */
    const glm::vec3& scale() const { return scale_; }

    /**
     * @brief Parses position, rotation, and scale from a raw fkYAML node.
     *
     * Expects the node to be a mapping with optional keys:
     *   position: { x, y, z }
     *   rotation: { x, y, z }
     *   scale:    { x, y, z }
     *
     * @param node fkYAML mapping node (from a CAMLMap).
     */
    template<typename Node>
    void set_from_node(const Node& node) {
        auto read_vec3 = [](const Node& n, const std::string& key) -> glm::vec3 {
            if (!n.contains(key)) return glm::vec3(0.0f);
            const auto& sub = n.at(key);
            float x = sub.contains("x") ? sub.at("x").template get_value<float>() : 0.0f;
            float y = sub.contains("y") ? sub.at("y").template get_value<float>() : 0.0f;
            float z = sub.contains("z") ? sub.at("z").template get_value<float>() : 0.0f;
            return glm::vec3(x, y, z);
        };

        position_        = read_vec3(node, "position");
        rotation_degrees_= read_vec3(node, "rotation");
        scale_           = read_vec3(node, "scale");
        if (scale_ == glm::vec3(0.0f)) scale_ = glm::vec3(1.0f); // guard
        mark_dirty();
    }

    // --- Hierarchy ---

    /**
     * @brief Sets the parent transform (non-owning pointer).
     * @param parent Pointer to the parent Transform, or nullptr for root.
     */
    void set_parent(Transform* parent) {
        parent_ = parent;
        mark_dirty();
    }

    /**
     * @brief Returns the parent transform pointer (may be nullptr).
     */
    Transform* parent() const { return parent_; }

    /**
     * @brief Registers a child transform for dirty propagation.
     * @param child Non-owning pointer to a child Transform.
     */
    void add_child(Transform* child) {
        children_.push_back(child);
    }

    /**
     * @brief Removes a previously registered child.
     * @param child Pointer to the child to remove.
     */
    void remove_child(Transform* child) {
        children_.erase(
            std::remove(children_.begin(), children_.end(), child),
            children_.end()
        );
    }

    // --- Matrix computation ---

    /**
     * @brief Returns the local transform matrix (position * rotation * scale).
     *
     * Recomputed only when dirty.
     *
     * @return Local matrix as glm::mat4.
     */
    const glm::mat4& get_local_matrix() const {
        if (dirty_) recompute_();
        return local_matrix_;
    }

    /**
     * @brief Returns the world transform matrix (parent_world * local).
     *
     * Recomputed lazily. Walks the parent chain if needed.
     *
     * @return World matrix as glm::mat4.
     */
    const glm::mat4& get_world_matrix() const {
        if (dirty_) recompute_();
        return world_matrix_;
    }

    /**
     * @brief Returns the cached world matrix WITHOUT recomputing it, even if dirty.
     *
     * Safe for unlimited concurrent readers, unlike get_world_matrix() (see
     * this class's thread-safety doc) -- call this only after
     * coopa::scene::TransformSystem's resolve pass has run this frame. In
     * debug builds, asserts the cache is not dirty, to catch a caller using
     * this before any resolve pass has run (e.g. on a freshly constructed
     * Transform, which starts dirty) or before TransformSystem is installed.
     */
    const glm::mat4& world_matrix() const {
        assert(!dirty_.load(std::memory_order_acquire) &&
               "Transform::world_matrix() read before a resolve pass -- "
               "call get_world_matrix() instead, or install coopa::scene::TransformSystem");
        return world_matrix_;
    }

    /**
     * @brief Returns whether this transform needs recomputation.
     */
    bool is_dirty() const { return dirty_.load(std::memory_order_relaxed); }

    /**
     * @brief Force-recomputes the world matrix. Called by the scene update pass.
     */
    void recompute() const { recompute_(); }

private:
    /**
     * @brief Marks this transform dirty and propagates to all children.
     *
     * Iterative (not recursive) so a deep hierarchy cannot blow the stack.
     * Uses a thread_local scratch buffer -- mark_dirty() is called on the
     * owner thread only (see this class's thread-safety doc), but different
     * scenes' owner threads may call it concurrently on unrelated Transform
     * graphs, so the buffer must not be shared across threads.
     */
    void mark_dirty() {
        thread_local std::vector<Transform*> stack;
        stack.clear();
        stack.push_back(this);
        while (!stack.empty()) {
            Transform* t = stack.back();
            stack.pop_back();
            t->dirty_.store(true, std::memory_order_relaxed);
            for (Transform* child : t->children_) stack.push_back(child);
        }
    }

    /**
     * @brief Recomputes local and world matrices.
     */
    void recompute_() const {
        // Local matrix: T * R * S
        glm::mat4 T = glm::translate(glm::mat4(1.0f), position_);
        glm::mat4 R = use_quat_
            ? glm::mat4_cast(rotation_quat_)
            : glm::eulerAngleZYX(
                  glm::radians(rotation_degrees_.z),
                  glm::radians(rotation_degrees_.y),
                  glm::radians(rotation_degrees_.x)
              );
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale_);
        local_matrix_ = T * R * S;

        // World matrix: parent_world * local (or just local if no parent)
        if (parent_) {
            world_matrix_ = parent_->get_world_matrix() * local_matrix_;
        } else {
            world_matrix_ = local_matrix_;
        }

        // Release, pairing with world_matrix()'s debug-only acquire load.
        // Note this flag's ordering is NOT what makes it safe for another
        // thread to read world_matrix() after this frame's resolve pass --
        // that cross-thread happens-before edge comes from the job handle
        // TransformSystem's resolve job contributes to (its counter's
        // release-on-completion / acquire-on-is_complete() pair, per
        // handle.h). This store only needs to be strong enough for the
        // same-thread debug assert to catch a caller reading a genuinely
        // stale cache after a real bug, not to establish visibility by itself.
        dirty_.store(false, std::memory_order_release);
    }

    glm::vec3 position_;           /**< Local position. */
    glm::vec3 rotation_degrees_;   /**< Local Euler rotation in degrees (XYZ). */
    glm::vec3 scale_;              /**< Local scale. */

    mutable glm::quat rotation_quat_{1.0f, 0.0f, 0.0f, 0.0f}; /**< Cached/authoritative quaternion
                                                                     form -- see rotation_quat(). */
    bool use_quat_ = false; /**< True once set_rotation_quat() has been called; cleared by
                                  set_rotation(vec3), so Euler authoring is unaffected. */

    Transform*              parent_;   /**< Non-owning parent pointer (may be nullptr). */
    std::vector<Transform*> children_; /**< Non-owning child pointers for dirty propagation. */

    mutable glm::mat4          local_matrix_  = glm::mat4(1.0f); /**< Cached local matrix. */
    mutable glm::mat4          world_matrix_  = glm::mat4(1.0f); /**< Cached world matrix. */
    mutable std::atomic<bool>  dirty_;                            /**< True when matrices need recomputation. */
};

} // namespace util
} // namespace coopa

#endif // COOPA_UTIL_TRANSFORM_H
