// editor/app/sculpt_preview.h -- the GPU mesh Sculpt Mode draws while a stroke reshapes it.
//
// Showing an edited mesh normally means exporting it to the engine's mesh YAML, welding and
// uploading it again (push_mesh_to_scene_) -- far too slow to repeat every frame of a brush
// stroke on a dense mesh. Sculpting never changes topology, so the render arrays (vertices,
// triangle indices) are built once; each frame only positions and normals are rewritten and
// re-uploaded through Mesh::update_vertices(), the per-frame-in-flight path cloth uses.

#pragma once

#include "../mesh/edit_mesh.h"
#include "../mesh/sculpt.h"

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/presentation/renderer.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace toy::editor {

class SculptPreview {
public:
    using GpuMesh = coopa::gfx::engine::data::Mesh;

    /** @brief Builds the render arrays and GPU buffers for `m` (throws on GPU failure). */
    void build(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, const EditMesh& m, const SculptCache& cache);

    /** @brief Re-uploads positions and normals into `frame_slot`'s buffer. */
    void update(const EditMesh& m, const SculptCache& cache, uint32_t frame_slot);

    const std::shared_ptr<GpuMesh>& mesh() const { return mesh_; }
    void reset();

private:
    struct Source { uint32_t v; int64_t flat_face; };   ///< flat_face < 0: smooth (vertex normal)

    void fill_(const EditMesh& m, const SculptCache& cache);

    std::vector<Source> src_;
    std::vector<coopa::gfx::engine::data::Vertex> verts_;
    std::shared_ptr<GpuMesh> mesh_;
};

}  // namespace toy::editor
