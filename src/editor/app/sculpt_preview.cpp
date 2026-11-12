#include "editor/app/sculpt_preview.h"

namespace toy {
namespace editor {

void SculptPreview::build(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, const EditMesh& m, const SculptCache& cache) {
    src_.clear();
    verts_.clear();
    std::vector<uint32_t> indices;
    std::map<std::pair<uint32_t, std::pair<float, float>>, uint32_t> smooth_ids;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        std::vector<uint32_t> ids;
        for (const auto& c : face.corners) {
            uint32_t id;
            if (face.smooth) {
                auto key = std::make_pair(c.v, std::make_pair(c.uv.x, c.uv.y));
                auto it = smooth_ids.find(key);
                if (it != smooth_ids.end()) { ids.push_back(it->second); continue; }
                id = static_cast<uint32_t>(src_.size());
                smooth_ids[key] = id;
                src_.push_back({c.v, -1});
            } else {
                id = static_cast<uint32_t>(src_.size());
                src_.push_back({c.v, static_cast<int64_t>(f)});
            }
            coopa::gfx::engine::data::Vertex vx{};
            vx.uv = c.uv;   // as stored (the mesh loader does not flip v either)
            vx.tangent = glm::vec4(1, 0, 0, 1);
            verts_.push_back(vx);
            ids.push_back(id);
        }
        for (size_t i = 1; i + 1 < ids.size(); ++i) {
            indices.push_back(ids[0]);
            indices.push_back(ids[i]);
            indices.push_back(ids[i + 1]);
        }
    }
    fill_(m, cache);
    mesh_ = std::make_shared<GpuMesh>(GpuMesh::from_arrays(device, allocator, verts_, indices,
                                                           coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT));
}

void SculptPreview::update(const EditMesh& m, const SculptCache& cache, uint32_t frame_slot) {
    if (!mesh_) return;
    fill_(m, cache);
    mesh_->update_vertices(verts_.data(), verts_.size(), frame_slot);
}

void SculptPreview::reset() { mesh_.reset(); src_.clear(); verts_.clear(); }

void SculptPreview::fill_(const EditMesh& m, const SculptCache& cache) {
    for (size_t i = 0; i < src_.size(); ++i) {
        verts_[i].position = m.positions[src_[i].v];
        if (src_[i].flat_face >= 0) {
            const glm::vec3 n = cache.face_n[static_cast<size_t>(src_[i].flat_face)];
            const float l = glm::length(n);
            verts_[i].normal = l > 1e-12f ? n / l : glm::vec3(0, 0, 1);
        } else {
            verts_[i].normal = cache.vert_n[src_[i].v];
        }
    }
}

} // namespace editor
} // namespace toy
