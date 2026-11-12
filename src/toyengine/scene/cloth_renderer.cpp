#include <toyengine/scene/cloth_renderer.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>

namespace toy {
namespace scene {

void ClothRenderer::rebuild_vertices() {
    if (!cloth_ || !renderer_ || vertices_.empty()) return;
    const coopa::physx::cloth::Cloth* sim = cloth_->cloth();
    if (!sim || sim->particles.size() != vertices_.size()) return;

    // Particles live in WORLD space (see ClothComponent's doc), but a MeshRenderer's vertices
    // are object-space and get multiplied by the owner's world matrix again at draw time.
    // Dividing it back out here is what keeps the owner's authored Transform meaningful --
    // moving or rotating the cloth object still moves the sheet -- instead of double-applying.
    glm::mat4 inv_world(1.0f);
    if (auto* tc = owner->get_transform()) {
        inv_world = glm::inverse(tc->transform().get_world_matrix());
    }

    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        vertices_[i].position = glm::vec3(inv_world * glm::vec4(sim->particles[i].position, 1.0f));
        vertices_[i].normal = glm::vec3(0.0f);
    }

    // Area-weighted vertex normals: the un-normalized cross product IS twice the triangle
    // area, so simply accumulating it weights each face by its area for free. That matters on
    // a deformed sheet, where a crease produces slivers next to full quads and an unweighted
    // average would let the slivers dominate the shading.
    for (std::size_t t = 0; t + 2 < sim->triangles.size(); t += 3) {
        const uint32_t i0 = sim->triangles[t], i1 = sim->triangles[t + 1], i2 = sim->triangles[t + 2];
        const glm::vec3 face = glm::cross(vertices_[i1].position - vertices_[i0].position,
                                          vertices_[i2].position - vertices_[i0].position);
        vertices_[i0].normal += face;
        vertices_[i1].normal += face;
        vertices_[i2].normal += face;
    }

    for (uint32_t y = 0; y < rows_; ++y) {
        for (uint32_t x = 0; x < columns_; ++x) {
            Vertex& v = vertices_[y * columns_ + x];
            const float n_len = glm::length(v.normal);
            v.normal = (n_len > 1e-6f) ? v.normal / n_len : glm::vec3(0.0f, 0.0f, 1.0f);

            // Tangent along +U, i.e. the grid's row direction -- the analytic answer, since the
            // UVs are the grid parameterisation. Central difference where possible so a crease
            // does not bias the tangent toward one side.
            const uint32_t xl = (x > 0) ? x - 1 : x;
            const uint32_t xr = (x + 1 < columns_) ? x + 1 : x;
            glm::vec3 tangent = vertices_[y * columns_ + xr].position -
                                vertices_[y * columns_ + xl].position;
            tangent -= v.normal * glm::dot(v.normal, tangent); // Gram-Schmidt against the normal
            const float t_len = glm::length(tangent);
            v.tangent = glm::vec4(t_len > 1e-6f ? tangent / t_len : glm::vec3(1.0f, 0.0f, 0.0f), 1.0f);
        }
    }
}

void ClothRenderer::upload(uint32_t frame_slot) {
    if (!mesh_ && !ensure_mesh_()) return;
    rebuild_vertices();
    mesh_->update_vertices(vertices_.data(), vertices_.size(), frame_slot);
}

void ClothRenderer::bind_siblings_() {
    if (!owner) return;
    if (!cloth_) cloth_ = owner->get_component<coopa::physx::components::ClothComponent>();
    if (!renderer_) renderer_ = owner->get_component<MeshRenderer>();
}

bool ClothRenderer::ensure_mesh_() {
    bind_siblings_();
    if (!cloth_ || !renderer_) return false;
    const coopa::physx::cloth::Cloth* sim = cloth_->cloth();
    if (!sim || sim->particles.empty() || sim->triangles.empty()) return false;

    columns_ = sim->columns;
    rows_ = sim->rows;
    vertices_.assign(sim->particles.size(), Vertex{});

    // UVs are fixed for the life of the sheet: they follow the grid's parameterisation, not
    // its deformed shape, which is what makes a checker texture stretch and compress with the
    // fabric exactly as a real print would.
    if (columns_ > 1 && rows_ > 1) {
        for (uint32_t y = 0; y < rows_; ++y) {
            for (uint32_t x = 0; x < columns_; ++x) {
                vertices_[y * columns_ + x].uv =
                    glm::vec2(static_cast<float>(x) / static_cast<float>(columns_ - 1),
                              static_cast<float>(y) / static_cast<float>(rows_ - 1));
            }
        }
    }

    rebuild_vertices();
    mesh_ = std::make_shared<Mesh>(
        Mesh::from_arrays(device_, allocator_, vertices_, sim->triangles, frames_in_flight_));

    // A synthetic asset id, prefixed to keep it out of the real-path namespace (see
    // AssetManager::create()'s doc). Published once, never per frame -- republishing retires a
    // payload each time. This component keeps its own non-const shared_ptr for mutation,
    // because an AssetHandle only ever hands out a const Mesh*.
    renderer_->set_mesh(assets_.create<Mesh>("runtime/cloth/" + owner->name(), mesh_));
    return true;
}

} // namespace scene
} // namespace toy
