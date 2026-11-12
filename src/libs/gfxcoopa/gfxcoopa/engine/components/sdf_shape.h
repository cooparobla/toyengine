/**
 * @file sdf_shape.h
 * @brief Component describing one signed-distance-field primitive.
 *
 * Placed on any descendant of an object carrying an SdfRenderer (see
 * sdf_renderer.h) -- its own TransformComponent's world matrix positions,
 * rotates and scales the primitive, exactly the way a MeshRenderer's mesh is
 * placed by the same transform system. SdfRenderer::collect_shapes() walks
 * the hierarchy and folds every SdfShape it finds into one signed-distance
 * field via `op`/`blend`, in child-list order.
 *
 * Note the one-per-object limit inherited from
 * coopa::scene::SceneObject::get_components_in_children(), which reads at
 * most one component of a given type per object (SceneObject::get_component()
 * under the hood): a single object can carry only one SdfShape. Use a child
 * object for each additional primitive.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_SDF_SHAPE_H
#define GFXCOOPA_ENGINE_COMPONENTS_SDF_SHAPE_H

#include <coopa/scene/component.h>
#include <glm/glm.hpp>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/// Which distance function an SdfShape evaluates. Matches gfx/sdf.glsl's
/// GFX_SDF_TYPE_* encoding (SdfShapeGPU::type_op_blend.x).
enum class SdfShapeType {
    Sphere, /**< params.x = radius. */
    Box,    /**< params = local half-extents. */
    Plane,  /**< params.x = signed offset along the local +Z half-space. */
};

/// How an SdfShape combines with the primitives already folded together
/// ahead of it in its SdfRenderer's child-list evaluation order. Matches
/// gfx/sdf.glsl's GFX_SDF_OP_* encoding (SdfShapeGPU::type_op_blend.y).
enum class SdfOperation {
    Union,     /**< Adds this shape's volume. */
    Subtract,  /**< Carves this shape's volume out of the accumulated result. */
    Intersect, /**< Keeps only the overlap with this shape's volume. */
};

/**
 * @class SdfShape
 * @brief One primitive (sphere/box/plane) in a signed-distance-field object.
 *
 * Combined with whatever SdfRenderer::collect_shapes() has accumulated so
 * far, via `op` and a polynomial smooth-min/max blend of strength `blend`
 * (0 = a hard boolean, matching gfx/sdf.glsl's gfx_sdf_smooth_union() family
 * degenerating to min()/max() at k=0).
 */
class SdfShape : public coopa::scene::Component {
public:
    SdfShape() = default;

    std::string type_name() const override { return "SdfShape"; }

    SdfShapeType type = SdfShapeType::Sphere;

    /// Shape-specific parameters, in the owning object's local space (i.e.
    /// pre-multiplied by nothing further -- the shape's own TransformComponent
    /// already places/rotates/scales it). See SdfShapeType for per-type meaning.
    glm::vec3 params = glm::vec3(1.0f);

    /// Subtracted from the raw distance, rounding corners/edges. 0 = sharp.
    float rounding = 0.0f;

    /// How this shape combines with the primitives folded together ahead of
    /// it (see SdfOperation).
    SdfOperation op = SdfOperation::Union;

    /// Smooth-blend strength (gfx_sdf_smooth_union/subtract/intersect's `k`).
    /// 0 = a hard boolean; higher values round the seam between this shape
    /// and the accumulated result. World units (roughly the blend radius).
    float blend = 0.0f;
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_SDF_SHAPE_H
