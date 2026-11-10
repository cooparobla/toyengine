/**
 * @file mesh_collider.h
 * @brief Static or kinematic triangle-mesh collider component.
 *
 * The AssetHandle is populated by physx_yaml.h's parser via `AssetManager::load<TriangleMesh>()`,
 * backed by `loaders::TriangleMeshLoader`. If the handle hasn't finished loading yet,
 * make_shape() returns a disabled Shape, so a scene referencing a mesh still being resolved
 * loads and renders inertly rather than crashing.
 *
 * Runtime geometry (a streamed terrain chunk, say) skips the asset system: set_mesh() also
 * takes a TriangleMesh the caller built (TriangleMesh::from_triangles()) and shares.
 */

#ifndef PHYSXCOOPA_COMPONENTS_MESH_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_MESH_COLLIDER_H

#include <physxcoopa/components/collider.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <coopa/asset/asset_handle.h>

#include <algorithm>
#include <memory>
#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class MeshCollider
 * @brief A static or kinematic triangle-mesh collider. Non-convex mesh colliders cannot be
 *        dynamic, matching Unity -- PhysicsSystem refuses to bind one to a non-kinematic
 *        Rigidbody. `convex` is stored but convex mesh colliders are not implemented.
 */
class MeshCollider : public Collider {
public:
    std::string type_name() const override { return "MeshCollider"; }

    const coopa::asset::AssetHandle<geometry::TriangleMesh>& mesh() const { return mesh_; }
    void set_mesh(coopa::asset::AssetHandle<geometry::TriangleMesh> handle) {
        mesh_ = std::move(handle);
        owned_.reset();
        bump_revision_();
    }

    /**
     * @brief Uses a mesh built at runtime instead of an asset. The collider shares ownership;
     *        whoever else may still read the shape after this collider is gone (an async
     *        navmesh bake) should keep its own reference a while longer.
     */
    void set_mesh(std::shared_ptr<const geometry::TriangleMesh> mesh) {
        owned_ = std::move(mesh);
        mesh_ = {};
        bump_revision_();
    }

    /** @brief The runtime mesh from set_mesh(shared_ptr), or null. */
    const std::shared_ptr<const geometry::TriangleMesh>& owned_mesh() const { return owned_; }

    bool convex() const { return convex_; }
    void set_convex(bool v) { convex_ = v; bump_revision_(); }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        const geometry::TriangleMesh* mesh = owned_ ? owned_.get() : mesh_.is_loaded() ? mesh_.get() : nullptr;
        if (mesh == nullptr) return collision::Shape{};
        // Max component of world scale, same rule as Sphere/Capsule -- see Shape::mesh_scale's
        // doc for why v1 supports uniform mesh scale only.
        float scale = std::max({world_scale.x, world_scale.y, world_scale.z});
        collision::Shape s = collision::Shape::make_mesh(mesh, center() * world_scale);
        s.mesh_scale = scale;
        return s;
    }

private:
    coopa::asset::AssetHandle<geometry::TriangleMesh> mesh_;
    std::shared_ptr<const geometry::TriangleMesh> owned_;
    bool convex_ = false;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_MESH_COLLIDER_H
