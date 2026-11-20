/**
 * @file character_cloth.h
 * @brief The CharacterCloth component: turns a sibling SkinnedMeshRenderer's garment mesh into
 *        an XPBD cloth driven by the mesh's painted cloth channels.
 *
 * Authoring data only, like physxcoopa's ClothComponent and toy::hair::Hair -- everything
 * derived (the sim cloth, the welded topology, per-frame skinned targets, bone collider shapes)
 * is owned by ClothMeshSystem and keyed by state_key(), because the editor destroys and
 * recreates a component on every inspector keystroke (see hair.h's identical note).
 *
 * The mesh and the rig come from the sibling SkinnedMeshRenderer; which vertices simulate, and
 * how far, comes from the mesh asset's `cloth:` channels (painted in the editor's Cloth Paint
 * mode -- see gfxcoopa's SkinnedMeshSource::ClothChannels). A mesh with no cloth paint leaves
 * this component inert, with a one-time log.
 *
 * ```yaml
 * - type: MeshRenderer
 *   material: { albedo: { r: 0.4, g: 0.2, b: 0.6 }, cull_backfaces: false }
 * - type: SkinnedMeshRenderer
 *   mesh_path: character_cape
 * - type: CharacterCloth
 *   mass_per_area: 0.2
 *   bend_compliance: 0.005
 *   anim_drive_compliance: 0.001
 *   bone_colliders:
 *     - { bone: spine, shape: capsule, radius: 0.14, half_height: 0.1, axis: 1 }
 * ```
 */

#ifndef TOYENGINE_CLOTH_CHARACTER_CLOTH_H
#define TOYENGINE_CLOTH_CHARACTER_CLOTH_H

#include <glm/glm.hpp>

#include <string>
#include <vector>

#include <coopa/scene/component.h>
#include <physxcoopa/cloth/cloth.h>

namespace toy {
namespace cloth {

/**
 * @struct BoneColliderSpec
 * @brief One primitive collider riding a rig bone, keeping the simulated garment off the body.
 *
 * `offset` and `rotation` are in the bone's local frame (bone axis +Y, the rig convention);
 * the capsule's own long axis therefore defaults to Y so a bare `{ bone, shape: capsule,
 * radius, half_height }` hugs a limb.
 *
 * **A capsule along the bone axis is centred ON the bone**, so `half_height` is half the bone's
 * length and `offset` is measured from the bone's middle. Every other shape -- a sphere, a box,
 * or a capsule deliberately laid across the bone -- sits at the bone's HEAD. Without that
 * distinction a limb capsule would cover its bone's first half and jut out past the joint,
 * which reads as cloth passing through the lower half of every limb.
 *
 * An entry naming a bone that `auto_colliders` already covered REPLACES every collider on it --
 * a bone can carry more than one, since each drawn part riding it is fitted separately -- so a
 * spec list is a set of corrections rather than the whole body. `radius: 0` on a capsule or
 * sphere removes the bone's colliders outright.
 */
struct BoneColliderSpec {
    std::string bone;             /**< Bone NAME in the rig's RigAsset. */
    std::string shape = "capsule"; /**< "sphere", "capsule" or "box". */
    float radius = 0.1f;          /**< Sphere/capsule radius, metres; 0 removes this bone's collider. */
    float half_height = 0.1f;     /**< Capsule cylindrical half-length, metres. */
    int axis = 1;                 /**< Capsule long axis in bone space: 0=X, 1=Y (bone axis), 2=Z. */
    glm::vec3 size{0.1f};         /**< Box half-extents, metres. */
    glm::vec3 offset{0.0f};       /**< Centre offset in the bone's local frame, metres. */
    glm::vec3 rotation{0.0f};     /**< Euler rotation in the bone's local frame, degrees. */
};

/**
 * @brief The capsule an auto collider gets for a bone `length` metres long, times `scale`.
 *
 * Returns `(radius, half_height)` in metres, the capsule lying along the bone's own +Y rest
 * axis with its centre at `length * 0.5`. The proportions match
 * `toy::scene::Ragdoll::auto_generate_()` -- radius a fifth of the bone, clamped so a stub
 * bone still gets something simulable and a long one does not become a barrel -- so a garment
 * and a ragdoll approximate the same body rather than two differently-shaped ones.
 *
 * A bone shorter than `k_auto_sphere_bone_length` gets a sphere instead (half_height 0): there
 * is no axis worth stretching a capsule along, and a stub bone is a wrist or a toe.
 */
glm::vec2 auto_bone_capsule(float length, float scale = 1.0f);

/** @brief Bones shorter than this (metres) get a sphere rather than a capsule. */
inline constexpr float k_auto_sphere_bone_length = 0.05f;

/**
 * @class CharacterCloth
 * @brief Authoring data for a painted-channel cloth over the sibling skinned mesh.
 */
class CharacterCloth : public coopa::scene::Component {
public:
    std::string type_name() const override { return "CharacterCloth"; }

    /** @brief The key ClothMeshSystem files this component's derived state under -- the owner's
     *         name, which survives the editor's destroy-and-recreate churn. */
    std::string state_key() const;

    /** @brief Areal density of the garment, kg/m^2. */
    float mass_per_area = 0.2f;

    /** @brief Solver tunables, re-read every frame (live tuning). The skin-cloth fields
     *         (anim_drive_compliance, max_distance_scale) live here too; `wind` is the authored
     *         base the weather's wind adds onto. */
    coopa::physx::cloth::ClothParams params;

    /**
     * @brief Metres of painted max_distance over which a vertex fades from fully skinned to
     *        fully simulated in the render blend -- the seam band.
     *
     * **0 by default, and that is not a disabled feature.** There is no seam to soften: a
     * particle painted at max_distance 0 is written from its skinned target every substep
     * (cloth_solver.h's stage 3b), so the simulated position IS the skinned position there and
     * departs from it continuously as the paint opens up. What the fade does instead is DISCARD
     * the solve over the band -- and the solve is the only thing that knows about collision, so a
     * vertex the solver lifted out of a hip is drawn back inside it. Measured on a garment with a
     * 12 mm leash at the hips and a 0.04 band: the fabric was drawn 2.6 cm into the body on every
     * frame of the walk.
     *
     * Raise it only to force a band of painted-free cloth to be DRAWN from the skinning anyway --
     * which costs exactly that collision response.
     */
    float max_distance_blend = 0.0f;

    /** @brief Multiplier on the weather system's wind at the owner (0 = ignore weather). */
    float weather_wind = 1.0f;

    /** @brief Collide against the physics world's broadphase shapes (plus the bone colliders). */
    bool collide_world = true;

    /**
     * @brief Fit a collider to EVERY bone of the rig -- to the geometry drawn on it where there
     *        is any, and to the bone's own length where there is not.
     *
     * On by default, because a garment that only collides with the few bones someone remembered
     * to list is a garment an elbow goes through.
     *
     * Where a bone CARRIES DRAWN GEOMETRY -- an object riding it on a BoneAttachment -- the
     * capsule is fitted around that mesh, so the garment collides with the body a viewer sees
     * rather than with a second model of it. auto_bone_capsule()'s bone-length rule is the
     * fallback for a bone nothing is drawn on (a skinned body mesh, or a bone that only carries
     * other bones); it is a guess about proportions and on a real character it is wrong by
     * centimetres in both directions, which is cloth either floating or laid through a limb.
     *
     * `bone_colliders` entries then correct individual bones (and `radius: 0` drops one); an
     * entry replaces EVERY collider on its bone. Turn this off for a garment whose body is
     * already described by world colliders, or to hand-author the whole set.
     */
    bool auto_colliders = true;

    /**
     * @brief Multiplier on every auto collider's radius -- the garment's standoff from the body.
     *
     * Above 1 holds the cloth further out (a padded coat), below 1 lets it sit closer (a thin
     * shirt). Hand-authored `bone_colliders` are not scaled: they are already the size they say.
     */
    float collider_scale = 1.0f;

    /** @brief Author-facing on/off; a disabled cloth holds the plain skinned pose. */
    bool enabled = true;

    /** @brief Primitive colliders riding rig bones; resolved by ClothMeshSystem. */
    std::vector<BoneColliderSpec> bone_colliders;

    /** @brief This frame's weather wind at the owner, written by toy::weather::WindSystem
     *         (pushed in, never pulled -- the Hair precedent). Added to `params.wind`. */
    glm::vec3 wind{0.0f};
};

} // namespace cloth
} // namespace toy

#endif // TOYENGINE_CLOTH_CHARACTER_CLOTH_H
