/**
 * @file cloth_renderer.h
 * @brief Draws a physxcoopa cloth as an ordinary shaded, shadow-casting mesh, by rewriting a
 *        dynamic GPU vertex buffer from the solver's particle positions once per frame.
 *
 * The split of responsibilities is the point: physxcoopa owns the simulation and knows nothing
 * about rendering, gfxcoopa owns the GPU mesh and knows nothing about cloth, and this component is
 * the one place the two meet. It reads `ClothComponent::cloth()` -- the solver's own particle
 * array, not a copy -- and writes a coopa::gfx::engine::data::Mesh built with one vertex per
 * PARTICLE (not per triangle corner), so a frame's upload is exactly `particle_count` vertices.
 *
 * Two mechanics are load-bearing and easy to get wrong:
 *
 *   - **Per-frame-in-flight buffers.** This engine's pipeline never waits per frame, so rewriting
 *     a single shared vertex buffer would race the GPU still reading last frame's. The mesh is
 *     created with `frames_in_flight` buffers and told which slot to write (see
 *     Mesh::update_vertices()). Same reason DebugLinePass keeps its own ring.
 *   - **When it runs.** upload() must be called after Scene::late_update() -- physics (phase 100)
 *     and TransformResolve (350) both have to have run -- and before the frame's command buffer is
 *     recorded. Engine::upload_dynamic_meshes_() is that window. It is deliberately NOT done in
 *     Component::late_update(), which has no way to know the in-flight slot.
 */

#ifndef TOYENGINE_SCENE_CLOTH_RENDERER_H
#define TOYENGINE_SCENE_CLOTH_RENDERER_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>

#include <physxcoopa/components/cloth.h>

namespace toy {
namespace scene {

/**
 * @class ClothRenderer
 * @brief Builds and maintains the GPU mesh for a sibling ClothComponent.
 *
 * Requires two siblings on the same SceneObject: a `Cloth` (the simulation) and a `MeshRenderer`
 * (the material and the draw). The MeshRenderer is authored with **no `mesh_path`** -- its mesh is
 * handed to it here instead. Give it a two-sided material (`cull_backfaces: false`): a draped
 * sheet shows both of its faces by definition, and the G-buffer shader already flips the normal
 * for back faces (`gbuffer_fs.glsl`), so both sides shade correctly.
 *
 * Example YAML:
 * @code
 * - type: MeshRenderer
 *   material: { albedo: { r: 0.8, g: 0.2, b: 0.3 }, roughness: 0.85, cull_backfaces: false }
 * - type: Cloth
 *   resolution: { x: 25, y: 25 }
 *   size: { x: 4.0, y: 4.0 }
 * - type: ClothRenderer
 * @endcode
 */
class ClothRenderer : public coopa::scene::Component {
public:
    using Mesh = coopa::gfx::engine::data::Mesh;
    using Vertex = coopa::gfx::engine::data::Vertex;
    using MeshRenderer = coopa::gfx::engine::components::MeshRenderer;

    ClothRenderer(coopa::gfx::core::Device& device,
                  coopa::gfx::memory::Allocator& allocator,
                  coopa::asset::AssetManager& assets,
                  uint32_t frames_in_flight)
        : device_(device), allocator_(allocator), assets_(assets),
          frames_in_flight_(frames_in_flight < 1u ? 1u : frames_in_flight) {}

    std::string type_name() const override { return "ClothRenderer"; }

    /**
     * @brief Caches the sibling Cloth and MeshRenderer. The GPU mesh is NOT built here.
     *
     * SceneLoader::load() calls Scene::start() at load time, before install_physics_system() has
     * ever run -- so at this point the sibling ClothComponent has no simulated cloth yet and there
     * is nothing to build a mesh from. Construction is therefore deferred to the first upload(),
     * by which time PhysicsSystem's reconcile pass has created the sheet. See ensure_mesh_().
     */
    void start() override {
        bind_siblings_();
    }

    /**
     * @brief Recomputes this frame's object-space vertex positions, normals and tangents from the
     *        solver's particles. Cheap enough to call unconditionally; does nothing if unbound.
     */
    void rebuild_vertices() {
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

    /**
     * @brief Builds the mesh if needed, refreshes the vertices, and uploads them into
     *        `frame_slot`'s buffer, making it the one the frame's draw will bind.
     *
     * The single per-frame entry point, called by Engine::upload_dynamic_meshes_() between
     * Scene::late_update() and PixelRenderPipeline::render(). A no-op until the sibling cloth
     * exists, so a scene that loads before physics binds simply starts drawing one frame later.
     *
     * @param frame_slot The renderer's current in-flight frame index.
     */
    void upload(uint32_t frame_slot) {
        if (!mesh_ && !ensure_mesh_()) return;
        rebuild_vertices();
        mesh_->update_vertices(vertices_.data(), vertices_.size(), frame_slot);
    }

    /** @brief True once the GPU mesh exists and is bound to the sibling MeshRenderer. */
    bool is_ready() const { return mesh_ != nullptr; }

private:
    /** @brief Resolves the two siblings this component drives. Retried from ensure_mesh_() so a
     *         ClothComponent added after start() still gets picked up. */
    void bind_siblings_() {
        if (!owner) return;
        if (!cloth_) cloth_ = owner->get_component<coopa::physx::components::ClothComponent>();
        if (!renderer_) renderer_ = owner->get_component<MeshRenderer>();
    }

    /**
     * @brief Allocates the dynamic GPU mesh from the sheet's current pose and binds it to the
     *        sibling MeshRenderer. Idempotent; returns false until the cloth actually exists.
     */
    bool ensure_mesh_() {
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

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager& assets_;
    uint32_t frames_in_flight_;

    coopa::physx::components::ClothComponent* cloth_ = nullptr;
    MeshRenderer* renderer_ = nullptr;
    std::shared_ptr<Mesh> mesh_;
    std::vector<Vertex> vertices_;
    uint32_t columns_ = 0;
    uint32_t rows_ = 0;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CLOTH_RENDERER_H
