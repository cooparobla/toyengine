/**
 * @file auto_weights.h
 * @brief Binds an EditMesh's vertex groups to a rig's bones -- the editor side of Blender's
 *        "With Automatic Weights".
 *
 * The weights themselves come from toy::scene::compute_auto_weights() (bone heat or
 * envelopes); this file is the adapter: it hands that function the mesh's WELDED positions and
 * its fan-triangulated faces, and writes the result back as vertex groups named after the
 * bones -- the form SkinnedMeshRenderer resolves against the rig by name, so a bind needs no
 * export step and no bone palette in the file.
 *
 * Which is also why a bind clears the mesh's `joints` / `joint_weights` /
 * `inverse_bind_matrices`: SkinnedMeshSource only derives its palette from `weights:` when the
 * file carries no explicit one, so an exporter's leftover palette would quietly win and the
 * generated weights would never reach the renderer.
 */

#ifndef TOYEDITOR_MESH_AUTO_WEIGHTS_H
#define TOYEDITOR_MESH_AUTO_WEIGHTS_H

#include "edit_mesh.h"

#include <toyengine/scene/auto_skin_weights.h>

#include <cstdint>
#include <string>
#include <vector>

namespace toy::editor {

using toy::scene::AutoWeightMethod;
using toy::scene::AutoWeightSettings;
using toy::scene::BoneSegments;

/** @brief What a bind did, for the status line. */
struct AutoWeightReport {
    size_t groups = 0;                      ///< Vertex groups written (one per bone).
    size_t weighted_vertices = 0;           ///< Vertices that came out with at least one weight.
    std::vector<std::string> unsolved;      ///< Bones the heat solve could not settle.
    bool fell_back_to_envelope = false;     ///< The mesh had no surface to conduct heat along.
    /// Groups the mesh already had that no bone claims -- their weights are left untouched, but
    /// they still count toward the renderer's per-vertex weight sum, so the caller says so.
    std::vector<std::string> foreign_groups;
};

/** @brief The mesh's faces as triangle corner indices into `positions` (fan, as the engine triangulates). */
std::vector<uint32_t> mesh_triangles(const EditMesh& m);

/**
 * @brief Generates weights for `bones` and writes them into `m` as groups named after them.
 *
 * Existing weights in those same groups are replaced (so re-running a bind with different
 * settings is idempotent), groups no bone claims are left alone, and the file's explicit
 * skinning palette is dropped -- see this file's doc.
 *
 * @param m        Mesh to weight in place.
 * @param bones    Deform bones, their segments in `m`'s object space.
 * @param settings See toy::scene::AutoWeightSettings.
 * @return What was written.
 */
AutoWeightReport apply_auto_weights(EditMesh& m, const std::vector<BoneSegments>& bones,
                                    const AutoWeightSettings& settings = {});

/**
 * @brief Blender's "With Empty Groups": one group per bone, no weights, for a mesh whose
 *        weights are going to be painted by hand.
 */
AutoWeightReport add_empty_bone_groups(EditMesh& m, const std::vector<BoneSegments>& bones);

} // namespace toy::editor

#endif // TOYEDITOR_MESH_AUTO_WEIGHTS_H
