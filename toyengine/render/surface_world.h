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
 *
 * Plain readonly storage buffers rather than images: host-visible, rewritten by memcpy, no
 * staging copies or layout transitions, readable in every stage -- what a few thousand floats
 * the CPU produces anyway need.
 */

#ifndef TOYENGINE_RENDER_SURFACE_WORLD_H
#define TOYENGINE_RENDER_SURFACE_WORLD_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/presentation/renderer.h>

namespace toy {
namespace render {

/** @brief What the engine hands the renderer each frame for the surface world set. */
struct SurfaceFrameState {
    // --- Snow / wetness (0 = none) ---
    float snow_cover = 0.0f;       ///< 0..1: how much snow lies on open, up-facing surfaces.
    float snow_depth = 0.3f;       ///< Metres of deep snow at full cover (the `snow` surface shader).
    bool snow_patch_hard = false;  ///< Round, crisp-edged patches instead of the soft noisy cover.
    float snow_patch_size = 1.5f;  ///< Hard patches: typical diameter (m).
    float wetness = 0.0f;
    glm::vec3 wind{0.0f};

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
};
static_assert(sizeof(SurfaceWorldUBO) == 144, "SurfaceWorldUBO must match world.glsl's std140 block");

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
