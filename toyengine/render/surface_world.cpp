#include <toyengine/render/surface_world.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace toy {
namespace render {

SurfaceWorldData::SurfaceWorldData(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator) {
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

void SurfaceWorldData::upload(uint32_t frame_slot, const SurfaceFrameState& s, const glm::vec3& camera_pos, float px_scale, float time) {
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
    u.snow_style = glm::vec4(s.snow_patch_hard ? 1.0f : 0.0f, std::max(s.snow_patch_size, 0.1f), 0.0f, 0.0f);
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

} // namespace render
} // namespace toy
