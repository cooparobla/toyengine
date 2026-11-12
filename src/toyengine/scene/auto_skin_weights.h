/**
 * @file auto_skin_weights.h
 * @brief Generates per-vertex skinning weights for a mesh from a rig's bone segments --
 *        Blender's "With Automatic Weights" (bone heat) and "With Envelope Weights".
 *
 * A pure function over arrays: welded vertex positions, triangle indices and one entry per
 * deform bone, all in the SAME space (the mesh's object space). It holds no GPU resources, no
 * scene and no assets, so it runs headless in a unit test and from the editor alike -- the
 * toyeditor is its only caller (binding a mesh to a rig bakes the result into the mesh file's
 * `weights:` vertex groups, which toy::scene::SkinnedMeshRenderer already resolves against the
 * rig by name). Nothing generates weights at runtime.
 *
 * The heat method is Baran & Popovic's bone-heat equilibrium, the algorithm Blender's
 * `heat_bone_weighting()` implements: treat each vertex as a point on a conducting surface,
 * inject heat at the vertices nearest a given bone and solve for the steady-state temperature
 * over the mesh. The temperature field IS the weight -- smooth along the surface, and crucially
 * smooth along the surface rather than through space, so a weight cannot leak across a gap that
 * the geometry does not bridge (the reason a hand near a hip does not get hip weights).
 *
 * The one deliberate departure from Blender is the solver. Nothing in this tree is a sparse
 * direct solver (no Eigen), so the system is solved with Jacobi-preconditioned conjugate
 * gradient instead of a Cholesky factorisation. CG needs a symmetric positive-definite matrix,
 * so the area-normalised Laplace-Beltrami rows are multiplied back by their vertex area -- an
 * equivalent system, symmetric by construction -- and the cotangent edge weights are clamped
 * non-negative. The clamp makes the matrix a diagonally dominant M-matrix, which both
 * guarantees CG converges and keeps every solved weight inside [0, 1] by the maximum
 * principle. The cost is slightly rounder weights than Blender's on meshes with very obtuse
 * triangles.
 */

#ifndef TOYENGINE_SCENE_AUTO_SKIN_WEIGHTS_H
#define TOYENGINE_SCENE_AUTO_SKIN_WEIGHTS_H

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace toy {
namespace scene {

/**
 * @struct BoneSegments
 * @brief One deform bone: the vertex group it owns, and the line segment(s) its flesh wraps.
 *
 * A joint drives everything from its own origin outward, so its segments run from the joint to
 * each of its child joints -- the same geometry the editor's bone glyphs draw, and the reason a
 * joint with two children (a pelvis with two hips) carries two segments. The distance from a
 * point to the bone is the smallest distance to any of them.
 */
struct BoneSegments {
    std::string name;                                           /**< The vertex group this bone gets. */
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;      /**< Head -> tail, mesh object space. */
};

/** @brief Which weight falloff to compute. */
enum class AutoWeightMethod {
    Heat = 0,   /**< Bone heat over the surface: Blender's "With Automatic Weights". */
    Envelope,   /**< Distance to the bone through space: Blender's "With Envelope Weights". */
};

/**
 * @struct AutoWeightSettings
 * @brief Knobs for compute_auto_weights(), defaulted to what a character rig wants.
 */
struct AutoWeightSettings {
    AutoWeightMethod method = AutoWeightMethod::Heat;
    /** Influences kept per vertex, strongest first. The engine skins four, so four is the
     *  default -- a mesh carrying more would be shown weights the renderer does not apply. */
    int   max_influences = 4;
    /** Weights under this are faded out and dropped, Blender's WEIGHT_LIMIT_START; the fade
     *  reaches zero at half of it. Stops a limb from very faintly dragging the whole body. */
    float weight_limit = 0.05f;
    bool  normalize = true;                 /**< Scale each vertex's kept weights to sum to 1. */
    int   smooth_iterations = 0;            /**< Edge-neighbour blur passes over the result. */
    float smooth_factor = 0.5f;             /**< How far each blur pass moves toward the neighbours. */
    /** Heat only: a bone only heats a vertex it can be seen from, tested against the mesh
     *  itself. This is what keeps an arm bone from weighting the chest it passes in front of. */
    bool  visibility = true;
    /** Envelope only: how far a bone reaches, as a multiple of its own length. */
    float envelope_falloff = 1.0f;
    /** Give a vertex that ended up with no weight at all its nearest bone outright. Blender
     *  leaves such a vertex unassigned; the engine would then hold it at its bind pose while
     *  the mesh around it moves, which reads as a tear, so filling it in is the default. */
    bool  fill_unweighted = true;
    int   cg_iterations = 500;              /**< Heat: per-bone conjugate-gradient iteration cap. */
    float cg_tolerance = 1e-6f;             /**< Heat: relative residual that counts as solved. */
};

/**
 * @struct AutoWeights
 * @brief The generated weights, plus what the generator could not do.
 */
struct AutoWeights {
    std::vector<std::string> groups;                              /**< Bone names, in the order given. */
    /** Per vertex (parallel to the positions passed in), its (group index, weight) pairs,
     *  strongest first, already limited and normalised. */
    std::vector<std::vector<std::pair<int, float>>> weights;
    /** Bones whose heat solve did not converge, so they got no weights -- Blender's "failed to
     *  find solution for one or more bones". Reported rather than hidden: the caller shows it. */
    std::vector<std::string> unsolved;
    /** The heat method fell back to envelope because the mesh has no usable surface to conduct
     *  along (fewer than one triangle, or every triangle degenerate). */
    bool fell_back_to_envelope = false;
};

/**
 * @brief The distance from `p` to the nearest point on `bone`, and that point.
 *
 * @param bone    Bone whose segments to measure against.
 * @param p       Query point, in the bones' space.
 * @param closest Optional out: the nearest point found on the bone.
 * @return The distance, or a huge value for a bone with no segments.
 */
float distance_to_bone(const BoneSegments& bone, const glm::vec3& p, glm::vec3* closest = nullptr);

/**
 * @brief Generates skinning weights for a mesh from its rig's bones.
 *
 * @param positions Welded vertex positions -- one entry per topological vertex, in the mesh's
 *                  object space. Weights come back parallel to this array.
 * @param triangles Triangle corner indices into `positions` (size a multiple of 3). The heat
 *                  method conducts along these; the envelope method ignores them.
 * @param bones     The deform bones, in the palette order the caller wants the groups in.
 * @param settings  See AutoWeightSettings.
 * @return The weights; `groups` is empty when `bones` is, or when `positions` is.
 */
AutoWeights compute_auto_weights(const std::vector<glm::vec3>& positions,
                                 const std::vector<uint32_t>& triangles,
                                 const std::vector<BoneSegments>& bones,
                                 const AutoWeightSettings& settings = {});

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_AUTO_SKIN_WEIGHTS_H
