/**
 * @file convex_collider.h
 * @brief Convex-hull collider component -- unlike MeshCollider, legal on a dynamic Rigidbody.
 *
 * Built either from a mesh asset's vertices (`hull_mesh`, resolved via `build_convex_hull()`
 * the first time the asset finishes loading -- cached, not rebuilt every make_shape() call) or
 * from a runtime-built hull via set_hull() (the path a sliced/fractured piece uses: its own
 * ConvexHull has no asset file to hold an AssetHandle to, so it is handed over as a shared_ptr
 * the same way MeshCollider::set_mesh(shared_ptr) already works for runtime meshes).
 *
 * This is deliberately a separate component from MeshCollider rather than a `convex: true` flag
 * on it: the two differ in every rule that matters (MeshCollider is static/kinematic-only;
 * ConvexCollider exists specifically to be dynamic), and folding them would mean a component
 * whose legality rules invert on a bool.
 */

#ifndef PHYSXCOOPA_COMPONENTS_CONVEX_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_CONVEX_COLLIDER_H

#include <physxcoopa/components/collider.h>
#include <physxcoopa/geometry/convex_hull.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <coopa/asset/asset_handle.h>

#include <algorithm>
#include <memory>
#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class ConvexCollider
 * @brief A convex-hull collider, legal on a static, kinematic, OR dynamic Rigidbody.
 */
class ConvexCollider : public Collider {
public:
    std::string type_name() const override { return "ConvexCollider"; }

    const coopa::asset::AssetHandle<geometry::TriangleMesh>& hull_mesh() const { return mesh_; }
    /** @brief Builds the hull from a mesh asset's vertices once the asset finishes loading. */
    void set_hull_mesh(coopa::asset::AssetHandle<geometry::TriangleMesh> handle) {
        mesh_ = std::move(handle);
        owned_.reset();
        invalidate_cache_();
        bump_revision_();
    }

    /**
     * @brief Uses a hull built at runtime (slicing, fracture) instead of a mesh asset. The
     *        collider shares ownership; whoever else may still read the shape after this
     *        collider is gone should keep its own reference a while longer.
     */
    void set_hull(std::shared_ptr<const geometry::ConvexHull> hull) {
        owned_ = std::move(hull);
        mesh_ = {};
        invalidate_cache_();
        bump_revision_();
    }

    /** @brief The runtime hull from set_hull(), or null. */
    const std::shared_ptr<const geometry::ConvexHull>& owned_hull() const { return owned_; }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        const geometry::ConvexHull* hull = resolve_hull_();
        if (hull == nullptr) return collision::Shape{};
        // Max component of world scale, same rule as Sphere/Capsule/MeshCollider -- see
        // Shape::hull_scale's doc for why v1 supports uniform hull scale only.
        float scale = std::max({world_scale.x, world_scale.y, world_scale.z});
        return collision::Shape::make_hull(hull, scale, center() * world_scale);
    }

private:
    void invalidate_cache_() {
        built_from_mesh_.reset();
        built_from_mesh_source_ = nullptr;
    }

    /** @brief The hull to collide with: the runtime hull if set, else the one built from
     *         `mesh_`'s vertices (lazily, cached against the loaded mesh's identity so a
     *         finished load builds the hull exactly once). Null while mesh_ is still loading,
     *         or if the mesh's point cloud is degenerate (see build_convex_hull()'s doc). */
    const geometry::ConvexHull* resolve_hull_() const {
        if (owned_) return owned_.get();
        if (!mesh_.is_loaded()) return nullptr;
        const geometry::TriangleMesh* mesh = mesh_.get();
        if (built_from_mesh_source_ != mesh) {
            built_from_mesh_source_ = mesh;
            auto built = std::make_shared<geometry::ConvexHull>();
            const std::vector<glm::vec3>& verts = mesh->vertices();
            built_from_mesh_ = geometry::build_convex_hull(verts.data(), verts.size(), *built) ? built : nullptr;
        }
        return built_from_mesh_.get();
    }

    coopa::asset::AssetHandle<geometry::TriangleMesh> mesh_;
    std::shared_ptr<const geometry::ConvexHull> owned_;
    mutable std::shared_ptr<const geometry::ConvexHull> built_from_mesh_;
    mutable const geometry::TriangleMesh* built_from_mesh_source_ = nullptr;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_CONVEX_COLLIDER_H
