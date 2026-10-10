#include <toyengine/render/passes/cloud_shadow_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

void CloudShadowPass::set_inputs(VkImageView weather, VkImageView shape, VkImageView detail, VkSampler noise_sampler,
                const std::vector<const coopa::gfx::memory::Buffer*>& cloud_frames,
                const std::vector<const coopa::gfx::memory::Buffer*>& flat_frames) {
    for (uint32_t i = 0; i < frames_; ++i) {
        auto& v = volumetric_stage_.set(0, i);
        v.bind_image(0, weather, noise_sampler);
        v.bind_image(1, shape, noise_sampler);
        v.bind_image(2, detail, noise_sampler);
        v.bind_buffer(3, *cloud_frames[i % cloud_frames.size()]);
        auto& f = flat_stage_.set(0, i);
        f.bind_image(0, weather, noise_sampler);
        f.bind_image(1, shape, noise_sampler);
        f.bind_buffer(2, *flat_frames[i % flat_frames.size()]);
    }
}

void CloudShadowPass::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    const VkClearColorValue clear{{1.0f, 1.0f, 1.0f, 1.0f}};
    target_.begin(cmd, clear);
    target_.end(cmd);
    initialized_ = true;
}

CloudShadowPass::Placement CloudShadowPass::place(glm::vec2 camera_xy, float side) {
    Placement pl;
    pl.side = std::max(side, 1.0f);
    const float texel = pl.side / static_cast<float>(kSize);
    pl.corner = glm::floor((camera_xy - 0.5f * pl.side) / texel) * texel;
    return pl;
}

void CloudShadowPass::render_(coopa::gfx::command::CommandBuffer& cmd, coopa::gfx::engine::passes::FullscreenStage& stage,
             uint32_t frame_slot, const Placement& pl, glm::vec3 light_to, int steps) {
    Push p;
    p.map = glm::vec4(pl.corner, pl.side, static_cast<float>(steps));
    p.light = glm::vec4(light_to, static_cast<float>(kSize));
    target_.begin(cmd);
    stage.bind(cmd, kSize, kSize, frame_slot % frames_);
    cmd.push_constants(coopa::gfx::ShaderStage::Fragment, p);
    stage.draw(cmd);
    target_.end(cmd);
    ++renders_;
}

coopa::gfx::engine::passes::FullscreenStageDesc CloudShadowPass::describe_volumetric_(
        const std::string& vert_spv, const std::string& frag_spv, uint32_t frames) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    d.owned_sets = {
        {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {3, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
    };
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(Push))}};
    d.instances = frames;
    return d;
}

coopa::gfx::engine::passes::FullscreenStageDesc CloudShadowPass::describe_flat_(
        const std::string& vert_spv, const std::string& frag_spv, uint32_t frames) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    d.owned_sets = {
        {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {2, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
    };
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(Push))}};
    d.instances = frames;
    return d;
}

} // namespace passes
} // namespace render
} // namespace toy
