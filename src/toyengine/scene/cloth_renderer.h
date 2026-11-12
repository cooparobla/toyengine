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


#include <gfxcoopa/engine/components/mesh_renderer.h>

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
    void rebuild_vertices();

    /**
     * @brief Builds the mesh if needed, refreshes the vertices, and uploads them into
     *        `frame_slot`'s buffer, making it the one the frame's draw will bind.
     *
     * The single per-frame entry point, called by Engine::upload_dynamic_meshes_() between
     * Scene::late_update() and ToyRenderPipeline::render(). A no-op until the sibling cloth
     * exists, so a scene that loads before physics binds simply starts drawing one frame later.
     *
     * @param frame_slot The renderer's current in-flight frame index.
     */
    void upload(uint32_t frame_slot);

    /** @brief True once the GPU mesh exists and is bound to the sibling MeshRenderer. */
    bool is_ready() const { return mesh_ != nullptr; }

private:
    /** @brief Resolves the two siblings this component drives. Retried from ensure_mesh_() so a
     *         ClothComponent added after start() still gets picked up. */
    void bind_siblings_();

    /**
     * @brief Allocates the dynamic GPU mesh from the sheet's current pose and binds it to the
     *        sibling MeshRenderer. Idempotent; returns false until the cloth actually exists.
     */
    bool ensure_mesh_();

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
