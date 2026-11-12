/**
 * @file heightfield_collider.h
 * @brief A static collider whose surface is a height grid (geometry/height_grid.h) -- a terrain's
 *        collision, read straight from its heights rather than from a triangle mesh asset.
 *
 * The shape it makes is a TriangleMesh shape in heightfield mode, so contacts, queries, sweeps,
 * the character motor and the navmesh bake all treat it like any static mesh -- and, like a
 * MeshCollider, it may not sit on a Dynamic Rigidbody.
 *
 * The grid is swapped wholesale (set_grid()) whenever the terrain changes; the old one is kept
 * alive a while longer, because a navmesh bake running on a worker may still be reading the
 * shape it snapshotted.
 */

#ifndef PHYSXCOOPA_COMPONENTS_HEIGHTFIELD_COLLIDER_H
#define PHYSXCOOPA_COMPONENTS_HEIGHTFIELD_COLLIDER_H

#include <physxcoopa/components/collider.h>
#include <physxcoopa/geometry/height_grid.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <deque>
#include <memory>
#include <string>

namespace coopa {
namespace physx {
namespace components {

class HeightfieldCollider : public Collider {
public:
    std::string type_name() const override { return "HeightfieldCollider"; }

    /** @brief Replaces the surface; the shape is rebuilt on the physics system's next frame. */
    void set_grid(std::shared_ptr<const geometry::HeightGrid> grid) {
        if (mesh_) {
            retired_.push_back(mesh_);
            while (retired_.size() > k_keep_retired) retired_.pop_front();
        }
        mesh_ = grid ? std::make_shared<const geometry::TriangleMesh>(geometry::TriangleMesh::from_height_grid(std::move(grid)))
                     : nullptr;
        bump_revision_();
    }

    /** @brief The current surface (null before the first set_grid()). */
    const geometry::HeightGrid* grid() const { return mesh_ ? mesh_->height_grid() : nullptr; }

    collision::Shape make_shape(const glm::vec3& world_scale) const override {
        if (!mesh_) return collision::Shape{};
        // Heights are metres: the grid is never scaled with its object.
        (void)world_scale;
        return collision::Shape::make_mesh(mesh_.get(), center());
    }

private:
    /// Surfaces kept alive after a swap -- enough for any in-flight async bake to finish.
    static constexpr std::size_t k_keep_retired = 8;

    std::shared_ptr<const geometry::TriangleMesh> mesh_;
    std::deque<std::shared_ptr<const geometry::TriangleMesh>> retired_;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_HEIGHTFIELD_COLLIDER_H
