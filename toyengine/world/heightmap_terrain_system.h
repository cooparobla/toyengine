/**
 * @file heightmap_terrain_system.h
 * @brief Drives every HeightmapTerrain: chunk objects, the GPU copies of its grid and splats,
 *        its layer texture arrays, its collider, and the per-frame bindings the renderer reads.
 *
 * ### Per frame (owner thread, order 61 -- after TileTerrain's 60, before Physics at 100)
 *
 *   1. GPU data. One height texture (R32F, metres) and two splat textures per HeightmapData,
 *      cached by data object -- so the editor re-parsing a terrain re-binds the same textures --
 *      and patched in place from the data's edit journal (HeightmapData::height_dirty_since()):
 *      a brush stroke is one sub-rectangle upload per frame, never a re-mesh.
 *   2. Layer arrays. Albedo and normal 2D arrays, cached by their file list and size, so changing
 *      a tiling or a tint re-uploads nothing.
 *   3. Chunks. One child SceneObject per chunk, all drawing the same shared flat patch mesh with
 *      the `heightmap_terrain` shader -- they batch into one instanced draw. A chunk's transform
 *      carries only its corner and its height range (for culling); the shader places vertices.
 *   4. Collider. A heightfield collider on a runtime child, rebuilt from the grid when it changes.
 *   5. Bindings. The terrain's slot, textures and numbers, handed to the renderer through
 *      Engine::sync_surface_state_() (SurfaceFrameState::terrains).
 *
 * Runtime children are not in the scene file: the editor's document never sees them, and saving
 * writes only the component (the grid and splats go to their own files, heightmap_store.h).
 */

#ifndef TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_SYSTEM_H
#define TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_SYSTEM_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <coopa/asset/asset_handle.h>
#include <coopa/scene/scene.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/texture.h>

#include <toyengine/render/surface_world.h>
#include <toyengine/world/heightmap_terrain.h>

namespace coopa {
namespace gfx {
namespace command { class CommandPool; }
} // namespace gfx
} // namespace coopa

namespace toy {
namespace render { class TextureArray; }
namespace world {

/** @brief After TileTerrain (60), before Physics (100) -- the collider must exist when it steps. */
inline constexpr int k_heightmap_terrain_system_order = 61;

/** @brief Extra height (m) above and below a chunk's grid range in its culling box: deep snow, slack. */
inline constexpr float k_heightmap_chunk_pad = 2.0f;

/**
 * @struct HeightmapGpu
 * @brief One HeightmapData's textures on the GPU and how current they are.
 */
struct HeightmapGpu {
    std::shared_ptr<coopa::gfx::engine::data::Texture> height;
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> height_handle;
    std::string height_id;
    std::shared_ptr<coopa::gfx::engine::data::Texture> splat0, splat1;
    int nx = 0, ny = 0, splat_size = 0;
    std::uint64_t uploaded_revision = 0;   ///< data->revision() the textures match
    std::uint64_t last_used_frame = 0;
};

/**
 * @struct HeightmapTerrainRuntime
 * @brief What the system keeps on each component (HeightmapTerrain::runtime).
 */
struct HeightmapTerrainRuntime {
    std::vector<coopa::scene::SceneObject*> chunks;   ///< cy * count.x + cx; owned by the terrain's object (null: not loaded)
    int live_chunks = 0;                              ///< Non-null entries of `chunks`
    glm::ivec2 chunk_count{0};
    HeightmapGeometry built_geometry;
    int built_patch = 0;
    std::uint64_t bounds_revision = 0;   ///< data revision the chunk boxes were fitted at
    std::shared_ptr<HeightmapGpu> gpu;
    std::shared_ptr<render::TextureArray> albedo, normal;
    int slot = -1;
    /// What the chunk materials were last written with; any change rewrites them.
    std::string material_signature;

    coopa::scene::SceneObject* collision = nullptr;
    std::uint64_t collider_revision = 0;       ///< data height_revision() the collider was built from
    std::uint64_t collider_seen_revision = 0;  ///< the latest revision seen while waiting to rebuild
    double collider_dirty_since = -1.0;        ///< seconds; rebuilds wait for edits to pause
};

/**
 * @class HeightmapTerrainSystem
 * @brief See the file comment.
 */
class HeightmapTerrainSystem : public coopa::scene::ISceneSystem {
public:
    HeightmapTerrainSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool, coopa::asset::AssetManager& assets);
    ~HeightmapTerrainSystem() override;

    const char* system_name() const override { return "HeightmapTerrain"; }
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief This frame's terrains for the renderer, in slot order. */
    const std::vector<render::TerrainFrameBinding>& frame_bindings() const { return bindings_; }

    /** @brief The shared patch mesh chunks draw (built on first use). */
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Mesh> patch_mesh(int resolution);

    /** @brief Builds the shared flat patch (object xy in [0, 1], z bounds [0, 1]). */
    static coopa::gfx::engine::data::MeshCpuData build_patch(int resolution);

    /** @brief The culling box of chunk (cx, cy) as a local position + scale (see the file comment). */
    static void chunk_box(const HeightmapData& data, int cx, int cy, glm::vec3& position, glm::vec3& scale);

    /**
     * @struct ChunkSettings
     * @brief A terrain's effective drawing settings, its `auto` values resolved.
     */
    struct ChunkSettings {
        int   patch = 4;              ///< Quads per chunk side.
        float view_distance = 0.0f;   ///< Chunks exist within this (m).
        float detail_distance = 0.0f; ///< Tessellation reaches this far (m).
        float shadow_distance = 0.0f; ///< Chunks cast within this (m); 0 = always.
        float max_factor = 64.0f;     ///< Tessellation cap (device-limited).
    };
    /** @brief Resolves `terrain`'s auto settings: `camera_far` is the main camera's far clip,
     *         `max_tess` the device's tessellation limit (0 = no tessellation). */
    static ChunkSettings resolve_settings(const HeightmapTerrain& terrain, float camera_far, float max_tess);

    /** @brief Distance (m) from `camera` to chunk (cx, cy)'s ground rectangle, in the xy plane. */
    static float chunk_distance(const HeightmapGeometry& g, const glm::vec3& origin, int cx, int cy, const glm::vec3& camera);

    /** @brief The renderer-side numbers of `terrain` drawn at `slot` (exposed for tests). */
    static render::TerrainGpuParams gpu_params(const HeightmapTerrain& terrain, const HeightmapTerrainRuntime& rt);

private:
    void tick_(HeightmapTerrain& terrain, coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx);
    std::shared_ptr<HeightmapGpu> gpu_for_(const HeightmapData& data);
    void upload_(HeightmapGpu& gpu, const HeightmapData& data);
    void layers_for_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt);
    /** @brief Spawns chunks that came within view distance, drops those well past it. */
    void stream_chunks_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene,
                        const ChunkSettings& s, const glm::vec3& camera);
    void spawn_chunk_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene, const ChunkSettings& s,
                      int cx, int cy);
    void apply_material_(const HeightmapTerrain& terrain, const HeightmapTerrainRuntime& rt, const ChunkSettings& s,
                         coopa::gfx::engine::components::MeshRenderer& mr) const;
    glm::vec3 camera_position_(const HeightmapTerrain& terrain) const;
    float camera_far_() const;
    void destroy_chunks_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt);
    void write_materials_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, const ChunkSettings& s);
    void fit_chunk_boxes_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt);
    void update_collider_(HeightmapTerrain& terrain, HeightmapTerrainRuntime& rt, coopa::scene::Scene& scene,
                          const coopa::scene::FrameContext& ctx);
    void evict_();

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
    coopa::asset::AssetManager& assets_;

    std::map<std::uint64_t, std::shared_ptr<HeightmapGpu>> gpu_cache_;   ///< by HeightmapData::uid()
    struct CachedArray {
        std::shared_ptr<render::TextureArray> array;
        std::uint64_t last_used_frame = 0;
    };
    std::map<std::string, CachedArray> array_cache_;
    std::map<int, coopa::asset::AssetHandle<coopa::gfx::engine::data::Mesh>> patches_;

    std::vector<render::TerrainFrameBinding> bindings_;
    std::uint64_t frame_ = 0;
    std::uint64_t next_id_ = 1;
    double time_ = 0.0;
    bool warned_slots_ = false;
};

/** @brief Installs the system (once per scene, before the first update). */
HeightmapTerrainSystem* install_heightmap_terrain_system(coopa::scene::Scene& scene, coopa::gfx::core::Device& device,
                                                         coopa::gfx::memory::Allocator& allocator,
                                                         coopa::gfx::command::CommandPool& cmd_pool,
                                                         coopa::asset::AssetManager& assets,
                                                         int order = k_heightmap_terrain_system_order);

inline HeightmapTerrainSystem* find_heightmap_terrain_system(const coopa::scene::Scene& scene) {
    return dynamic_cast<HeightmapTerrainSystem*>(scene.find_system("HeightmapTerrain"));
}

/** @brief Every heightmap terrain in `scene` as a world-space query view. */
std::vector<HeightmapView> heightmap_views(const coopa::scene::Scene& scene);

/** @brief The first heightmap terrain covering world xy, or an empty view. */
HeightmapView heightmap_at(const coopa::scene::Scene& scene, const glm::vec3& world);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_HEIGHTMAP_TERRAIN_SYSTEM_H
