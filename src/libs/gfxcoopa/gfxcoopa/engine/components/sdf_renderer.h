/**
 * @file sdf_renderer.h
 * @brief Component defining the 3D bounds of a raymarched signed-distance-field object.
 *
 * An SdfRenderer does not itself describe any geometry -- it collects
 * SdfShape components (see sdf_shape.h) from its own object and every
 * descendant, in child-list order, and marks the owning object's subtree as
 * one raymarchable object. This is the same pattern MeshRenderer uses for
 * triangle meshes, except the "mesh" here is an arbitrary hierarchy of
 * primitives instead of a single asset: each SdfShape's own
 * TransformComponent places it, so moving/rotating/scaling a child object
 * moves/rotates/scales that primitive using the engine's ordinary transform
 * system, with no SDF-specific placement fields duplicated here.
 *
 * bounds_center/bounds_extent define a local-space box, relative to the
 * SdfRenderer's own object's transform. The render pipeline transforms this
 * box's 8 corners into clip space to find both the screen-space (pre-upscale)
 * rectangle the object's pixels raymarch within and the AABB the in-shader
 * ray/box slab test clips the march to -- see gfx_sdf_aabb_intersect() in
 * gfx/sdf.glsl. Pixels outside that rectangle never invoke the raymarch loop
 * at all: this bounds box IS the performance story for the whole system, so
 * it should be kept as tight as the authored shapes allow, not left at the
 * (1,1,1) default for anything non-trivial.
 *
 * material reuses PBRMaterial verbatim (see mesh_renderer.h) so an SDF object
 * has exactly the same material surface as a mesh: albedo/metallic/roughness/
 * ao/emissive, plus alpha/alpha_mode selecting between the deferred G-buffer
 * path (Opaque/Mask) and the forward BLEND path (Blend) -- the same split
 * MeshRenderer's material drives for triangle geometry.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_SDF_RENDERER_H
#define GFXCOOPA_ENGINE_COMPONENTS_SDF_RENDERER_H

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/sdf_shape.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @class SdfRenderer
 * @brief Declares the 3D bounds of a raymarched SDF object and gathers its shapes.
 */
class SdfRenderer : public coopa::scene::Component {
public:
    SdfRenderer() = default;

    std::string type_name() const override { return "SdfRenderer"; }

    /// Local-space bounds box (relative to this object's own transform) used
    /// to derive the screen-space raymarch rectangle and the in-shader AABB
    /// clip. See the file doc -- keep this tight around the authored shapes.
    glm::vec3 bounds_center = glm::vec3(0.0f);
    glm::vec3 bounds_extent = glm::vec3(1.0f);

    /// Same material surface as MeshRenderer -- see PBRMaterial's doc.
    /// alpha_mode selects the G-buffer (Opaque/Mask) or forward BLEND path,
    /// exactly like a mesh renderer's material does.
    PBRMaterial material;

    /// Whether this object casts directional/point shadows. Mirrors
    /// MeshRenderer's implicit shadow-casting (always true there); explicit
    /// here since a raymarch is comparatively expensive to repeat in every
    /// shadow pass. A BLEND material only casts a shadow at alpha == 1.0,
    /// same binary rule as MeshRenderer -- see ToyRenderPipeline's shadow
    /// recording for why (a single hard depth compare can't express a
    /// partial shadow).
    bool cast_shadows = true;

    /// Mirrors MeshRenderer::affects_reflection_probes -- static SDF objects
    /// contribute to baked reflection probe captures; moving ones should
    /// disable this and rely on SSR alone.
    bool affects_reflection_probes = true;

    /// Raymarch step budget for this object's main (G-buffer/forward) draw.
    /// Clamped at record time to the pipeline's global sdf_max_steps.
    int max_steps = 64;

    /// Surface hit threshold, in world units -- the march stops once the
    /// field value drops below this.
    float surface_epsilon = 0.001f;

    /// Finite-difference step used by the tetrahedral normal estimator
    /// (see gfx_sdf_normal_renderer() in toyengine's gfx/sdf_scene_body.glsl).
    float normal_epsilon = 0.002f;

    /// Default smooth-blend strength applied when a collected SdfShape
    /// leaves its own `blend` at 0 -- lets an artist set one blend radius
    /// for the whole object instead of repeating it on every child shape.
    float smoothing = 0.0f;

    /// Collects every SdfShape on this object and its descendants, in the
    /// same pre-order the transform hierarchy already walks
    /// (SceneObject::get_components_in_children()). See sdf_shape.h's doc
    /// for the one-shape-per-object limit this inherits.
    std::vector<SdfShape*> collect_shapes() const {
        if (!owner) return {};
        return owner->get_components_in_children<SdfShape>();
    }
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_SDF_RENDERER_H
