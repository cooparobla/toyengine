// editor/app/paint_preview.h -- the GPU mesh Vertex Paint / Weight Paint draw while painting.
//
// Like SculptPreview (sculpt_preview.h): the render arrays are built once per topology and only
// rewritten -- here the colours, plus positions and normals for undo -- through
// Mesh::update_vertices(), the per-frame-in-flight path. One render vertex per face corner, so
// the display never depends on how corners happen to share colours, and painting never changes
// the layout.
//
// What is shown is packed for the editor-only `editor_paint` surface shader
// (assets/shaders/editor_paint.vert): uv = (red, green), tangent.w = 1 + blue -- the corner's
// vertex colour
// (white where the mesh has no colour attribute) or the active vertex group's weight as
// Blender's heatmap (blue 0 -> red 1; vertices outside the group read as 0).

#pragma once

#include "../mesh/edit_mesh.h"
#include "../mesh/paint.h"
#include "../mesh/sculpt.h"

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/presentation/renderer.h>

#include <memory>
#include <vector>

namespace toy::editor {

class PaintPreview {
public:
    using GpuMesh = coopa::gfx::engine::data::Mesh;

    /** @brief What the preview shows: the colour attribute, or one vertex group's weights. */
    struct Show {
        bool weights = false;
        uint32_t group = 0;
    };

    /** @brief Builds the render arrays and GPU buffers for `m` (throws on GPU failure). */
    void build(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, const EditMesh& m,
               const SculptCache& cache, Show show);

    /** @brief Rewrites positions, normals and shown colours into `frame_slot`'s buffer. */
    void update(const EditMesh& m, const SculptCache& cache, Show show, uint32_t frame_slot);

    const std::shared_ptr<GpuMesh>& mesh() const { return mesh_; }
    void reset();

private:
    struct Source { uint32_t face; uint32_t corner; };

    void fill_(const EditMesh& m, const SculptCache& cache, Show show);

    std::vector<Source> src_;
    std::vector<coopa::gfx::engine::data::Vertex> verts_;
    std::shared_ptr<GpuMesh> mesh_;
};

}  // namespace toy::editor
