/**
 * @file surface_world.h
 * @brief The "surface world" descriptor set: world-space data every opaque surface pass can
 *        read in its vertex, tessellation and fragment stages -- the tessellation view (camera
 *        position and pixel scale), the snow / wetness state, the weather's precipitation
 *        occlusion map ("is this point open to the sky?") and the snow trench field objects
 *        press into.
 *
 * Bound as set 2 of the G-buffer pipelines and set 1 of the shadow pipelines (one shared layout,
 * see layout()); shaders reach it through assets/shaders/gfx/surface/world.glsl. One set per
 * frame in flight, refilled by upload() from SurfaceFrameState (Engine fills it every frame, see
 * Engine::sync_surface_state_()):
 *
 *   binding 0  UBO     SurfaceWorldUBO (below; mirrors world.glsl's block)
 *   binding 1  SSBO    float occl_heights[nx * ny] -- the highest surface under each cell (the
 *                      weather's GroundProbe); NaN-free (the fallback is written in)
 *   binding 2  SSBO    uint snow_trench[] -- the trench field, two unorm16 cells per word,
 *                      toroidally addressed (cell (ix, iy) lives at (ix mod n, iy mod n))
 */
#ifndef TOYENGINE_RENDER_SURFACE_WORLD_H
#define TOYENGINE_RENDER_SURFACE_WORLD_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/env/env_field.h>

#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/presentation/renderer.h>

namespace toy {
namespace render {

/** @brief Heightmap terrains a frame can draw at once (world.glsl's gfx_world.terrain[]). */
inline constexpr int kMaxSurfaceTerrains = 4;
/**
 * @brief Surface layers a terrain's params carry (world.glsl's GfxTerrain arrays).
 *
 * Sixteen because a tiles-mode terrain names its surface in a 4-bit nibble against a 4 x 4
 * palette atlas (toyengine/world/tile_types.h), and the built-in tile layers already fill
 * thirteen of them. Painting by WEIGHT still reaches only the first kSurfaceTerrainSplatLayers,
 * which is what two RGBA8 splat maps hold.
 */
inline constexpr int kSurfaceTerrainLayers = 16;

/** @brief Layers the two RGBA8 splat maps carry a weight for, one channel each. */
inline constexpr int kSurfaceTerrainSplatLayers = 8;

/**
 * @brief std140 mirror of world.glsl's GfxTerrain: one heightmap terrain's numbers -- where its
 *        grid sits, how big a texel is, and each paint layer's look. Textures are not here: the
 *        height grid is the chunks' material displacement map and the splats / layer arrays are
 *        G-buffer set 3 (toy::render::TerrainGpuRegistry).
 */
struct alignas(16) TerrainGpuParams {
    glm::vec4  origin{0.0f};   ///< xyz world position of the grid's (0, 0) corner at height 0, w chunk size (m)
    glm::vec4  cell{1.0f};     ///< xy metres per height texel, zw terrain size (m)
    glm::ivec4 dims{0};        ///< x nx, y ny, z layer count, w flags (TerrainGpuFlag)
    glm::vec4  misc{0.0f};     ///< x height (m), y splat texels per side, zw reserved
    glm::vec4  layer[kSurfaceTerrainLayers]{};      ///< x 1/tiling, y roughness, z metallic, w normal strength
    glm::vec4  layer_tri[kSurfaceTerrainLayers]{};  ///< x 1 = triplanar, y sharpness, z 1 = has normal map
    glm::vec4  tint[kSurfaceTerrainLayers]{};       ///< rgb albedo multiplier
};
static_assert(sizeof(TerrainGpuParams) == (4 + 3 * kSurfaceTerrainLayers) * 16,
              "TerrainGpuParams must match world.glsl's GfxTerrain");

/** @brief TerrainGpuParams::dims.w bits. */
enum TerrainGpuFlag : int {
    kTerrainDeepSnow = 1,  ///< Raise and carve deep snow (snow_height.glsl) on open, flat ground.
    kTerrainPuddles  = 2,  ///< Rain gathers in hollows.
};

/**
 * @brief std140 mirror of world.glsl's GfxEnvSource: one local environmental influence
 *        (toyengine/env/env_source.h) as the surface stages read it.
 *
 * Temperature is deliberately absent even though `surface.yzw` is free: nothing on the GPU
 * reads it, and the CPU field (toy::env::EnvField) remains the authority for gameplay and snow
 * melt. `wetness` stays in `params.w` rather than moving here, so the existing CPU/GPU parity
 * test keeps testing the same bytes.
 */
struct alignas(16) EnvSourceGpu {
    glm::vec4 pos_radius{0.0f};  ///< xyz world centre, w radius (m); w <= 0 = an unused slot
    glm::vec4 wind{0.0f};        ///< Directional: xyz m/s. Radial: x outward, y vertical. w turbulence
    glm::vec4 params{0.0f};      ///< x falloff, y disc half-height (0 = unbounded), z flags, w wetness
    glm::vec4 surface{0.0f};     ///< x snow cover added here, yzw spare
};
static_assert(sizeof(EnvSourceGpu) == 4 * 16, "EnvSourceGpu must match world.glsl's GfxEnvSource");

/** @brief EnvSourceGpu::params.z bits. */
enum EnvSourceGpuFlag : int {
    kEnvShapeDisc    = 1,  ///< A cylinder about +Z rather than a sphere.
    kEnvModeRadial   = 2,  ///< Wind aims away from the +Z axis rather than along one vector.
    kEnvBlendMax     = 4,  ///< Only the strongest such source contributes.
    kEnvBlendOverride = 8, ///< Replaces the combined result inside the shape.
};

class TextureArray;

/**
 * @brief One terrain's textures for a frame -- what set 3 binds for chunks whose displacement map
 *        is `height`. Shared ownership: the registry keeps them alive while its sets reference them.
 */
struct TerrainFrameBinding {
    const coopa::gfx::engine::data::Texture* height = nullptr;   ///< Key: the chunks' displacement map.
    std::shared_ptr<coopa::gfx::engine::data::Texture> splat0, splat1;
    std::shared_ptr<TextureArray> albedo, normal;
    TerrainGpuParams params;
};

/** @brief What the engine hands the renderer each frame for the surface world set. */
struct SurfaceFrameState {
    // --- Snow / wetness (0 = none) ---
    float snow_cover = 0.0f;       ///< 0..1: how much snow lies on open, up-facing surfaces.
    float snow_depth = 0.3f;       ///< Metres of deep snow at full cover (the `snow` surface shader).
    bool snow_patch_hard = false;  ///< Round, crisp-edged patches instead of the soft noisy cover.
    float snow_patch_size = 1.5f;  ///< Hard patches: typical diameter (m).
    float wetness = 0.0f;
    glm::vec3 wind{0.0f};          ///< Mean wind (m/s); gusts from wind_gust / wind_time (toyengine/weather/wind.h).
    float wind_gust = 0.0f;
    float wind_time = 0.0f;

    // --- Precipitation occlusion map (the weather's GroundProbe) ---
    /// Highest surface per cell, nx * ny, row-major in y; NaN = nothing (use `occl_fallback`).
    /// Null = no map: everything counts as open to the sky.
    std::shared_ptr<const std::vector<float>> occl_heights;
    glm::vec2 occl_origin{0.0f};
    float occl_cell = 1.0f;
    int occl_nx = 0, occl_ny = 0;
    float occl_fallback = 0.0f;

    // --- Snow trench field (toy::world::SnowField) ---
    const uint32_t* trench_words = nullptr;   ///< n * n / 2 words; null = no trenches.
    int trench_n = 0;                          ///< Cells a side (even).
    float trench_cell = 0.1f;                  ///< Metres per cell.
    glm::ivec2 trench_window{0};               ///< World cell index of the window's min corner.
    float trench_scale = 1.0f;                 ///< Metres a full unorm16 (65535) trench is deep.
    uint64_t trench_version = 0;               ///< Bumped whenever the words change.


    // --- Local environmental influences (toy::env::EnvSystem) ---
    /// The frame's environment field. Its global wind/gust/wetness terms duplicate the fields
    /// above (the weather fills both); its `source[]` is what only the GPU fold can use.
    env::EnvField env;

    // --- Heightmap terrains (toy::world::HeightmapTerrainSystem); index = the chunks' slot ---
    std::vector<TerrainFrameBinding> terrains;
};

/** @brief std140 mirror of world.glsl's SurfaceWorldUBO. */
struct alignas(16) SurfaceWorldUBO {
    glm::vec4  tess_view{0.0f};       ///< xyz main camera position, w pixel scale (0.5 * render height * proj[1][1])
    glm::vec4  snow{0.0f};            ///< x cover, y depth (m), z wetness, w time (s)
    glm::vec4  wind{0.0f};            ///< xyz wind (m/s), w trench scale (m per unorm 1)
    glm::vec4  occl{0.0f};            ///< xy origin, z cell, w 1 = map valid
    glm::ivec4 occl_dims{0};          ///< x nx, y ny
    glm::vec4  occl_fallback{0.0f};   ///< x fallback height
    glm::vec4  field{0.0f};           ///< z cell, w 1 = trench field valid
    glm::ivec4 field_dims{0};         ///< x n, yz window min cell index
    glm::vec4  snow_style{0.0f};      ///< x 1 = hard-edged patches, y patch size (m)
    glm::vec4  wind_gust{0.0f};       ///< x gust strength, y gust clock (s) -- see toyengine/weather/wind.h
    glm::ivec4 terrain_count{0};      ///< x terrains in use, y local env sources in use
    TerrainGpuParams terrain[kMaxSurfaceTerrains]{};
    /// Appended AFTER terrain[] on purpose: inserting ahead of it would shift every terrain
    /// offset, so a CPU/GLSL mismatch would silently corrupt the terrain params instead of
    /// only the new data.
    EnvSourceGpu env[env::kMaxEnvSources]{};
    /// Also appended after env[], for the same reason: inserting ahead of terrain[] or env[]
    /// would shift every one of their offsets.
};
static_assert(sizeof(SurfaceWorldUBO) == 176 + kMaxSurfaceTerrains * sizeof(TerrainGpuParams) +
                                         env::kMaxEnvSources * sizeof(EnvSourceGpu),
              "SurfaceWorldUBO must match world.glsl's std140 block");

class SurfaceWorldData {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kMaxOcclCells = 192 * 192;     ///< Precipitation map capacity.
    static constexpr uint32_t kMaxTrenchWords = 512 * 512 / 2; ///< Trench field capacity.

    SurfaceWorldData(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator);

    SurfaceWorldData(const SurfaceWorldData&) = delete;
    SurfaceWorldData& operator=(const SurfaceWorldData&) = delete;

    const coopa::gfx::pipeline::DescriptorSetLayout& layout() const { return *layout_; }
    const coopa::gfx::pipeline::DescriptorSet& set(uint32_t frame_slot) const { return *sets_[frame_slot % kFrames]; }

    /**
     * @brief Fills `frame_slot`'s buffers from `s` and the main camera's tessellation view.
     *        The trench words are copied only when their version changed since this slot last
     *        saw them (they are the big buffer).
     */
    void upload(uint32_t frame_slot, const SurfaceFrameState& s, const glm::vec3& camera_pos, float px_scale, float time);

    /** @brief What the last upload() wrote (tests / diagnostics). */
    const SurfaceWorldUBO& last() const { return last_; }

private:
    std::vector<coopa::gfx::memory::Buffer> ubo_, occl_, trench_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> pool_;
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> sets_;
    std::vector<uint64_t> trench_versions_;
    std::vector<float> scratch_;
    SurfaceWorldUBO last_{};
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_SURFACE_WORLD_H
