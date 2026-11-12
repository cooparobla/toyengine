#include "editor/app/paint_preview.h"

namespace toy {
namespace editor {

void PaintPreview::build(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, const EditMesh& m,
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

void PaintPreview::update(const EditMesh& m, const SculptCache& cache, Show show, uint32_t frame_slot) {
    if (!mesh_) return;
    fill_(m, cache, show);
    mesh_->update_vertices(verts_.data(), verts_.size(), frame_slot);
}

void PaintPreview::reset() { mesh_.reset(); src_.clear(); verts_.clear(); }

void PaintPreview::fill_(const EditMesh& m, const SculptCache& cache, Show show) {
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

} // namespace editor
} // namespace toy
