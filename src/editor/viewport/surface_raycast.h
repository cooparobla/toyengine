/**
 * @file surface_raycast.h
 * @brief Finding the surface under a viewport ray -- its point AND its normal -- over whatever
 *        kinds of geometry a caller feeds in.
 *
 * `EditorApp::pick_object()` answers "which object is under the cursor"; `surface_point_()`
 * answers "where", brute-force over every triangle in the scene and throwing the normal away.
 * An instance brush needs the normal (to orient what it places), needs many rays per dab
 * (so a BVH rather than a face loop), and needs to treat terrain and ordinary meshes alike.
 *
 * This type is **pure geometry**: it walks nothing and owns nothing. The caller offers
 * candidates one at a time -- `consider_mesh()` for a mesh object, `consider_terrain()` for
 * either kind of terrain -- and the raycaster keeps the nearest. That is what keeps it
 * testable without an editor, a document or a device.
 *
 * @code
 * SurfaceRaycaster rc(origin, dir);
 * for (auto& [id, obj] : objects) rc.consider_mesh(id, mesh, bvh, world);
 * for (auto& t : terrains) rc.consider_terrain(t.id, t.view);
 * if (const SurfaceHit* h = rc.best()) place_something(h->point, h->normal);
 * @endcode
 */

#ifndef TOYEDITOR_VIEWPORT_SURFACE_RAYCAST_H
#define TOYEDITOR_VIEWPORT_SURFACE_RAYCAST_H

#include <optional>

#include <glm/glm.hpp>

#include <toyengine/world/terrain_field.h>

#include "editor/app/editor_types.h"
#include "editor/mesh/mesh_bvh.h"

namespace toy::editor {

/** @brief Where a ray met a surface, and which way that surface faces. */
struct SurfaceHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};   ///< Unit length, facing back along the ray.
    ObjectId  object = 0;                  ///< 0 for a hit on something with no object id.
    float     t = 0.0f;                    ///< Distance along the ray.
};

/**
 * @class SurfaceRaycaster
 * @brief Nearest-surface search along one ray. See the file comment.
 */
class SurfaceRaycaster {
public:
    SurfaceRaycaster(glm::vec3 origin, glm::vec3 dir, float max_t = 1e5f);

    /**
     * @brief Tests one mesh object, through its BVH, in its own local space.
     * @param world The object's world matrix; a non-invertible one (a zero scale) is skipped.
     */
    void consider_mesh(ObjectId id, const EditMesh& mesh, const TriangleBVH& bvh, const glm::mat4& world);

    /**
     * @brief Tests one terrain, in either mode.
     *
     * `TerrainField` has no raycast of its own -- the two planes under it do, in different
     * terms -- so this marches the ray against `ground_at()` and bisects the crossing. Mode
     * agnostic by construction, which is the point: a brush must not care whether the ground
     * below it is terraced tiles or a smooth grid.
     *
     * @param step Marching step in metres; the default resolves a tile-sized feature.
     */
    void consider_terrain(ObjectId id, const toy::world::TerrainFieldView& view, float step = 0.5f);

    /** @brief Offers an explicit hit (a drag plane, a fallback) to the same nearest-wins test. */
    void consider_point(ObjectId id, const glm::vec3& point, const glm::vec3& normal);

    /** @brief The nearest hit so far, or null. */
    const SurfaceHit* best() const { return best_ ? &*best_ : nullptr; }

    glm::vec3 origin() const { return origin_; }
    glm::vec3 direction() const { return dir_; }

private:
    void take_(const SurfaceHit& hit);

    glm::vec3 origin_{0.0f};
    glm::vec3 dir_{0.0f, 0.0f, -1.0f};
    float     max_t_ = 1e5f;
    std::optional<SurfaceHit> best_;
};

/**
 * @brief The ray/plane intersection a brush falls back to when it hits nothing.
 * @return The point, or nullopt when the ray runs parallel to the plane.
 */
std::optional<glm::vec3> ray_plane(glm::vec3 origin, glm::vec3 dir, float plane_z);

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_SURFACE_RAYCAST_H
