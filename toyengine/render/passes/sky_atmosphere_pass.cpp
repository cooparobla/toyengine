#include <toyengine/render/passes/sky_atmosphere_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

SkyAtmospherePass::AtmosphereGpu SkyAtmospherePass::AtmosphereGpu::of(const SkyAtmosphereMedia& m) {
    AtmosphereGpu g;
    g.rayleigh = glm::vec4(m.rayleigh_scattering, m.rayleigh_scale_height);
    g.mie = glm::vec4(m.mie_scattering, m.mie_scale_height);
    g.mie_ext = glm::vec4(m.mie_extinction, m.mie_g);
    g.ozone = glm::vec4(m.ozone_absorption, m.bottom_radius);
    g.ground = glm::vec4(m.ground_albedo, m.top_radius);
    return g;
}

SkyAtmospherePass::SkyAtmospherePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  const std::string& vert_spv, const std::string& transmittance_spv,
                  const std::string& multiscatter_spv, const std::string& view_spv)
    : linear_(coopa::gfx::engine::util::Sampler::linear(device)),
      transmittance_(device, allocator, kTransmittanceW, kTransmittanceH, coopa::gfx::Format::RGBA16_Sfloat,
                     coopa::gfx::engine::targets::kColorOnly),
      multiscatter_(device, allocator, kMultiscatterSize, kMultiscatterSize, coopa::gfx::Format::RGBA16_Sfloat,
                    coopa::gfx::engine::targets::kColorOnly),
      view_(device, allocator, kViewW, kViewH, coopa::gfx::Format::RGBA16_Sfloat,
            coopa::gfx::engine::targets::kColorOnly),
      transmittance_stage_(device, transmittance_.render_pass_object(), describe_(vert_spv, transmittance_spv, 0, sizeof(AtmosphereGpu))),
      multiscatter_stage_(device, multiscatter_.render_pass_object(), describe_(vert_spv, multiscatter_spv, 1, sizeof(AtmosphereGpu))),
      view_stage_(device, view_.render_pass_object(), describe_(vert_spv, view_spv, 2, sizeof(ViewParams))) {
    multiscatter_stage_.set(0).bind_image(0, transmittance_.color_view_typed(), linear_);
    view_stage_.set(0).bind_image(0, transmittance_.color_view_typed(), linear_);
    view_stage_.set(0).bind_image(1, multiscatter_.color_view_typed(), linear_);
}

void SkyAtmospherePass::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 1.0f}};
    transmittance_.begin(cmd, zero); transmittance_.end(cmd);
    multiscatter_.begin(cmd, zero); multiscatter_.end(cmd);
    view_.begin(cmd, zero); view_.end(cmd);
    initialized_ = true;
}

void SkyAtmospherePass::update_luts(coopa::gfx::command::CommandBuffer& cmd, const SkyAtmosphereMedia& media) {
    if (luts_valid_ && media == lut_media_) return;
    const AtmosphereGpu g = AtmosphereGpu::of(media);
    transmittance_.begin(cmd);
    transmittance_stage_.draw(cmd, kTransmittanceW, kTransmittanceH, coopa::gfx::ShaderStage::Fragment, g);
    transmittance_.end(cmd);
    multiscatter_.begin(cmd);
    multiscatter_stage_.draw(cmd, kMultiscatterSize, kMultiscatterSize, coopa::gfx::ShaderStage::Fragment, g);
    multiscatter_.end(cmd);
    lut_media_ = media;
    luts_valid_ = true;
    ++lut_renders_;
}

void SkyAtmospherePass::render_view(coopa::gfx::command::CommandBuffer& cmd, const ViewParams& params) {
    view_.begin(cmd);
    view_stage_.draw(cmd, kViewW, kViewH, coopa::gfx::ShaderStage::Fragment, params);
    view_.end(cmd);
}

coopa::gfx::engine::passes::FullscreenStageDesc SkyAtmospherePass::describe_(const std::string& vert_spv, const std::string& frag_spv,
                                                                 int samplers, size_t push_bytes) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    if (samplers > 0) {
        std::vector<coopa::gfx::pipeline::DescriptorBinding> b;
        for (int i = 0; i < samplers; ++i) {
            b.push_back({static_cast<uint32_t>(i), DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1});
        }
        d.owned_sets = {b};
    }
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(push_bytes)}};
    return d;
}

} // namespace passes
} // namespace render
} // namespace toy
