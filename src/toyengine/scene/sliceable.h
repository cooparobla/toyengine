/**
 * @file sliceable.h
 * @brief Marks an object as cuttable by DestructionSystem::slice_object(): the style the two
 *        halves are spawned with.
 *
 * No physics, no GPU -- a plain data component, same contract as Destructible
 * (toyengine/scene/destructible.h), which already carries the same keys for an object that can
 * be both cut and shattered. Put Sliceable on an object that should only ever be CUT (never
 * shatter on impact); put both on an object that does either. The only geometry it ever carries
 * is `cached_vertices`/`cached_indices`, and only on a spawned half -- never author those.
 *
 * Example YAML:
 * @code
 * - type: Sliceable
 *   interior_material: materials/wood_end_grain   # empty reuses the surface material
 *   density: 700                                  # kg/m^3, oak is ~700
 *   collider: hull                                # or "boxes"
 *   max_slices: 2                                 # a half may itself be cut once more
 * @endcode
 */

#ifndef TOYENGINE_SCENE_SLICEABLE_H
#define TOYENGINE_SCENE_SLICEABLE_H

#include <cstdint>
#include <string>
#include <vector>

#include <coopa/scene/component.h>

#include <toyengine/destruct/mesh_solid.h>

namespace toy {
namespace scene {

/**
 * @class Sliceable
 * @brief An object DestructionSystem::slice_object() may cut into two rigid halves.
 */
class Sliceable : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Sliceable"; }

    /** @brief materials/<name>.yaml for a cut's newly-made interior face; empty reuses the
     *         surface material. */
    std::string interior_material;

    /** @brief Density used to weigh a half from its volume, kg/m^3 (oak is ~700, concrete ~2400). */
    float density = 700.0f;

    /** @brief Seconds a half lives before it is cleaned up; 0 keeps it for ever. */
    float piece_lifetime = 10.0f;

    /** @brief Seconds a half spends sinking out of sight before it goes; 0 makes it vanish. */
    float piece_fade = 1.0f;

    /** @brief physics_materials/<name>.yaml for the halves; empty uses the engine default. */
    std::string physics_material;

    /** @brief A half's collider: `"hull"` (a convex hull built from its own vertices, the
     *         default) or `"boxes"` (destruct::fit_boxes(), the fallback for a degenerate
     *         hull -- a near-flat sliver). */
    std::string collider = "hull";

    /** @brief How much of its box a half must fill before it collides as that one box, 0..1 --
     *         only consulted in the `"boxes"` collider path. */
    float collider_fill = 0.6f;

    /** @brief Most collision boxes one half may use in the `"boxes"` collider path. */
    int max_boxes = 4;

    /** @brief How many more times a half of THIS object may itself be cut. A spawned half
     *         carries this minus one on its own Sliceable; 0 refuses the cut entirely. */
    int max_slices = 1;

    /**
     * @brief A spawned half's own geometry, in its local (principal) frame -- the same vertices
     *        its GPU mesh was built from. Empty on an authored object, which has a mesh ASSET
     *        slice_object() can re-read (MeshRenderer::mesh_path()); a half has no such file,
     *        so DestructionSystem::slice_object() cuts this cached copy instead whenever it is
     *        set. Not for scenes to author.
     */
    std::vector<destruct::Vertex>        cached_vertices;
    std::vector<std::uint32_t>           cached_indices;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_SLICEABLE_H
