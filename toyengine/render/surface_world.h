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

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/presentation/renderer.h>

namespace toy {
namespace render {

/** @brief What the engine hands the renderer each frame for the surface world set. */
struct SurfaceFrameState {
    // --- Snow / wetness (0 = none) ---
    float snow_cover = 0.0f;       ///< 0..1: how much snow lies on open, up-facing surfaces.
    float snow_depth = 0.3f;       ///< Metres of deep snow at full cover (the `snow` surface shader).
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
};
static_assert(sizeof(SurfaceWorldUBO) == 128, "SurfaceWorldUBO must match world.glsl's std140 block");

class SurfaceWorldData {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kMaxOcclCells = 192 * 192;     ///< Precipitation map capacity.
    static constexpr uint32_t kMaxTrenchWords = 512 * 512 / 2; ///< Trench field capacity.

    SurfaceWorldData(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator) {
        using coopa::gfx::ShaderStage;
        ShaderStage stages = ShaderStage::Vertex | ShaderStage::Fragment;
        if (device.supports_tessellation()) stages = stages | ShaderStage::TessControl | ShaderStage::TessEval;
        for (uint32_t i = 0; i < kFrames; ++i) {
            ubo_.push_back(coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(SurfaceWorldUBO)));
            occl_.push_back(coopa::gfx::memory::Buffer::storage(device, allocator, sizeof(float) * kMaxOcclCells));
            trench_.push_back(coopa::gfx::memory::Buffer::storage(device, allocator, sizeof(uint32_t) * kMaxTrenchWords));
        }
        layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, stages)
                .storage_buffer(1, stages)
                .storage_buffer(2, stages)
                .build(device));
        pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kFrames).build(device));
        for (uint32_t i = 0; i < kFrames; ++i) {
            sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_));
            sets_[i]->bind_buffer(0, ubo_[i]);
            sets_[i]->bind_storage_buffer(1, occl_[i]);
            sets_[i]->bind_storage_buffer(2, trench_[i]);
        }
        // Valid contents from frame 0: zeroed maps (an all-zero trench field and an empty
        // occlusion map are both "nothing here").
        std::vector<float> zeros_f(kMaxOcclCells, 0.0f);
        std::vector<uint32_t> zeros_u(kMaxTrenchWords, 0u);
        for (uint32_t i = 0; i < kFrames; ++i) {
            SurfaceWorldUBO u{};
            ubo_[i].upload(&u, sizeof(u));
            occl_[i].upload(zeros_f.data(), sizeof(float) * zeros_f.size());
            trench_[i].upload(zeros_u.data(), sizeof(uint32_t) * zeros_u.size());
        }
        trench_versions_.assign(kFrames, 0);
    }

    SurfaceWorldData(const SurfaceWorldData&) = delete;
    SurfaceWorldData& operator=(const SurfaceWorldData&) = delete;

    const coopa::gfx::pipeline::DescriptorSetLayout& layout() const { return *layout_; }
    const coopa::gfx::pipeline::DescriptorSet& set(uint32_t frame_slot) const { return *sets_[frame_slot % kFrames]; }

    /**
     * @brief Fills `frame_slot`'s buffers from `s` and the main camera's tessellation view.
     *        The trench words are copied only when their version changed since this slot last
     *        saw them (they are the big buffer).
     */
    void upload(uint32_t frame_slot, const SurfaceFrameState& s, const glm::vec3& camera_pos, float px_scale, float time) {
        const uint32_t k = frame_slot % kFrames;
        SurfaceWorldUBO u{};
        u.tess_view = glm::vec4(camera_pos, px_scale);
        u.snow = glm::vec4(std::clamp(s.snow_cover, 0.0f, 1.0f), std::max(0.0f, s.snow_depth), s.wetness, time);
        u.wind = glm::vec4(s.wind, s.trench_scale);
        const int cells = s.occl_nx * s.occl_ny;
        const bool occl_ok = s.occl_heights && cells > 0 && static_cast<uint32_t>(cells) <= kMaxOcclCells &&
                             s.occl_heights->size() >= static_cast<size_t>(cells);
        if (occl_ok) {
            scratch_.resize(static_cast<size_t>(cells));
            for (int i = 0; i < cells; ++i) {
                const float h = (*s.occl_heights)[static_cast<size_t>(i)];
                scratch_[static_cast<size_t>(i)] = h != h ? s.occl_fallback : h;   // NaN -> fallback
            }
            occl_[k].upload(scratch_.data(), sizeof(float) * scratch_.size());
            u.occl = glm::vec4(s.occl_origin, s.occl_cell, 1.0f);
            u.occl_dims = glm::ivec4(s.occl_nx, s.occl_ny, 0, 0);
        }
        u.occl_fallback = glm::vec4(s.occl_fallback, 0.0f, 0.0f, 0.0f);
        const bool trench_ok = s.trench_words && s.trench_n > 0 &&
                               static_cast<uint32_t>(s.trench_n) * static_cast<uint32_t>(s.trench_n) / 2u <= kMaxTrenchWords;
        if (trench_ok) {
            if (trench_versions_[k] != s.trench_version) {
                trench_[k].upload(s.trench_words, sizeof(uint32_t) * static_cast<size_t>(s.trench_n) * static_cast<size_t>(s.trench_n) / 2u);
                trench_versions_[k] = s.trench_version;
            }
            u.field = glm::vec4(0.0f, 0.0f, s.trench_cell, 1.0f);
            u.field_dims = glm::ivec4(s.trench_n, s.trench_window.x, s.trench_window.y, 0);
        }
        ubo_[k].upload(&u, sizeof(u));
        last_ = u;
    }

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
