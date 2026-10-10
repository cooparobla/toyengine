#include <toyengine/render/passes/cloud_overlay_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

CloudOverlayPass::FlatFrame CloudOverlayPass::flat_frame_of(const CloudFrameState& st) {
    const double size = std::max(static_cast<double>(st.flat_size), 0.5);
    auto wrap = [&](double speed, double period) {
        const glm::dvec2 v = st.offset * speed;
        const double p = size * period;
        return glm::vec2(v - glm::floor(v / p) * p);
    };
    FlatFrame f;
    f.layer = glm::vec4(st.altitude, st.thickness, static_cast<float>(size), st.flat_opacity);
    f.drift0 = glm::vec4(wrap(1.0, kGroupPeriod), wrap(kWarpSpeed, kWarpPeriod));
    f.drift1 = glm::vec4(wrap(1.0, kPuff0Period), wrap(kPuff1Speed, kPuff1Period));
    f.look = glm::vec4(st.coverage, st.flat_light_bands, st.flat_outline, st.fade);
    f.flow = glm::vec4(st.flat_turbulence, st.flat_phase, st.time, 0.0f);
    return f;
}

CloudOverlayPass::CloudOverlayPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, VkRenderPass render_pass,
                 uint32_t frames_in_flight, const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& light_layout, const std::string& vert_spv,
                 const std::string& flat_spv, const std::string& composite_spv)
    : frames_(std::max(frames_in_flight, 1u)),
      nearest_(coopa::gfx::engine::util::Sampler::nearest(device)),
      flat_stage_(device, coopa::gfx::detail::RawRenderPass{render_pass},
                  describe_flat_(camera_layout, light_layout, vert_spv, flat_spv, frames_)),
      composite_stage_(device, coopa::gfx::detail::RawRenderPass{render_pass},
                       describe_composite_(vert_spv, composite_spv)) {
    for (uint32_t i = 0; i < frames_; ++i) {
        flat_ubos_.push_back(std::make_unique<coopa::gfx::memory::Buffer>(
            coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(FlatFrame))));
    }
}

void CloudOverlayPass::set_inputs(coopa::gfx::TextureView g_normal, coopa::gfx::TextureView g_position,
                VkImageView weather, VkImageView shape, VkSampler noise_sampler,
                coopa::gfx::TextureView volumetric_layer) {
    for (uint32_t i = 0; i < frames_; ++i) {
        auto& f = flat_stage_.set(0, i);
        f.bind_image(0, g_normal, nearest_);
        f.bind_image(1, g_position, nearest_);
        f.bind_image(2, weather, noise_sampler);
        f.bind_image(3, shape, noise_sampler);
        f.bind_buffer(4, *flat_ubos_[i]);
    }
    auto& c = composite_stage_.set(0);
    c.bind_image(0, g_normal, nearest_);
    c.bind_image(1, g_position, nearest_);
    c.bind_image(2, volumetric_layer, nearest_);
}

void CloudOverlayPass::draw_flat(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot,
               const coopa::gfx::pipeline::DescriptorSet& camera_set,
               const coopa::gfx::pipeline::DescriptorSet& light_set, const glm::mat4& inv_view_proj,
               uint32_t width, uint32_t height) const {
    flat_stage_.bind(cmd, width, height, frame_slot % frames_);
    cmd.bind_descriptor_set(camera_set, 0);
    cmd.bind_descriptor_set(light_set, 1);
    cmd.push_constants(coopa::gfx::ShaderStage::Fragment, inv_view_proj);
    flat_stage_.draw(cmd);
}

void CloudOverlayPass::draw_composite(coopa::gfx::command::CommandBuffer& cmd, const CompositePush& p, uint32_t width, uint32_t height) const {
    composite_stage_.bind(cmd, width, height);
    cmd.push_constants(coopa::gfx::ShaderStage::Fragment, p);
    composite_stage_.draw(cmd);
}

coopa::gfx::engine::passes::FullscreenStageDesc CloudOverlayPass::describe_flat_(
        const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
        const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
        const std::string& vert_spv, const std::string& frag_spv, uint32_t frames) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    d.leading_layouts = {&camera_layout, &light_layout};
    d.owned_sets = {
        {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {4, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
    };
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(glm::mat4))}};
    d.blend = coopa::gfx::pipeline::BlendMode::PremultipliedAlpha;
    d.instances = frames;
    return d;
}

coopa::gfx::engine::passes::FullscreenStageDesc CloudOverlayPass::describe_composite_(const std::string& vert_spv,
                                                                           const std::string& frag_spv) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    d.owned_sets = {
        {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
    };
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(CompositePush))}};
    d.blend = coopa::gfx::pipeline::BlendMode::PremultipliedAlpha;
    return d;
}

} // namespace passes
} // namespace render
} // namespace toy
