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
               const SculptCache& cache, Show show) {
        src_.clear();
        verts_.clear();
        std::vector<uint32_t> indices;
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            const auto& face = m.faces[f];
            const uint32_t first = static_cast<uint32_t>(src_.size());
            for (uint32_t k = 0; k < face.corners.size(); ++k) {
                src_.push_back({f, k});
                verts_.push_back(coopa::gfx::engine::data::Vertex{});
            }
            for (uint32_t i = 1; i + 1 < face.corners.size(); ++i) {
                indices.push_back(first);
                indices.push_back(first + i);
                indices.push_back(first + i + 1);
            }
        }
        if (verts_.empty() || indices.empty()) throw std::runtime_error("nothing to paint: the mesh has no faces");
        fill_(m, cache, show);
        mesh_ = std::make_shared<GpuMesh>(GpuMesh::from_arrays(device, allocator, verts_, indices,
                                                               coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT));
    }

    /** @brief Rewrites positions, normals and shown colours into `frame_slot`'s buffer. */
    void update(const EditMesh& m, const SculptCache& cache, Show show, uint32_t frame_slot) {
        if (!mesh_) return;
        fill_(m, cache, show);
        mesh_->update_vertices(verts_.data(), verts_.size(), frame_slot);
    }

    const std::shared_ptr<GpuMesh>& mesh() const { return mesh_; }
    void reset() { mesh_.reset(); src_.clear(); verts_.clear(); }

private:
    struct Source { uint32_t face; uint32_t corner; };

    void fill_(const EditMesh& m, const SculptCache& cache, Show show) {
        for (size_t i = 0; i < src_.size(); ++i) {
            const Face& face = m.faces[src_[i].face];
            const Corner& c = face.corners[src_[i].corner];
            verts_[i].position = m.positions[c.v];
            glm::vec3 n = face.smooth ? cache.vert_n[c.v] : cache.face_n[src_[i].face];
            const float l = glm::length(n);
            verts_[i].normal = l > 1e-12f ? n / l : glm::vec3(0, 0, 1);
            glm::vec3 col(1.0f);
            if (show.weights) col = weight_heatmap(m.weight(c.v, show.group));
            else if (m.has_colors) col = glm::vec3(c.color);
            verts_[i].uv = glm::vec2(col.r, col.g);
            verts_[i].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f + col.b);
        }
    }

    std::vector<Source> src_;
    std::vector<coopa::gfx::engine::data::Vertex> verts_;
    std::shared_ptr<GpuMesh> mesh_;
};

}  // namespace toy::editor
