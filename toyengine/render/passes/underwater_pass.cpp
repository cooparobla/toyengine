#include <toyengine/render/passes/underwater_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

void UnderwaterPass::set_source_images(coopa::gfx::TextureView scene_color, coopa::gfx::TextureView g_normal,
                       coopa::gfx::TextureView g_position,
                       const coopa::gfx::engine::util::Sampler& linear_sampler) {
    stage_.set(0).bind_image(0, scene_color, linear_sampler);
    stage_.set(0).bind_image(1, g_normal, nearest_sampler_);
    stage_.set(0).bind_image(2, g_position, nearest_sampler_);
}

coopa::gfx::engine::passes::FullscreenStageDesc UnderwaterPass::describe_(const std::string& vert_spv,
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
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(Params))}};
    return d;
}

} // namespace passes
} // namespace render
} // namespace toy
