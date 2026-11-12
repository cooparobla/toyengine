#include <toyengine/render/passes/contact_shadow_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

ContactShadowPass::ContactShadowPass(coopa::gfx::core::Device& device,
                  coopa::gfx::memory::Allocator& allocator,
                  const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                  const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                  uint32_t width, uint32_t height,
                  const std::string& vert_spv,
                  const std::string& march_spv,
                  const std::string& resolve_spv)
    : device_(device), width_(width), height_(height)
{
    using namespace coopa::gfx;

    // NEAREST throughout: this is an occlusion mask sampled at texel centres, and the
    // G-buffer taps the march makes are discrete per-pixel data that must never be blended
    // across a silhouette. The resolve's history read is the one exception — it samples at a
    // reprojected, non-texel-aligned UV.
    SamplerDesc nearest_clamp;
    nearest_clamp.min = nearest_clamp.mag = Filter::Nearest;
    nearest_clamp.mipmap  = MipmapMode::Nearest;
    nearest_clamp.address = AddressMode::ClampToEdge;
    nearest_sampler_ = std::make_unique<engine::util::Sampler>(device, nearest_clamp);

    SamplerDesc linear_clamp = nearest_clamp;
    linear_clamp.min = linear_clamp.mag = Filter::Linear;
    linear_sampler_  = std::make_unique<engine::util::Sampler>(device, linear_clamp);

    march_target_ = std::make_unique<engine::targets::OffscreenTarget>(
        device, allocator, width, height, kFormat, engine::targets::kColorOnly);
    resolved_target_ = std::make_unique<engine::targets::OffscreenTarget>(
        device, allocator, width, height, kFormat, engine::targets::kColorOnly);
    history_image_ = std::make_unique<memory::Image>(
        device, allocator, width, height, kFormat,
        coopa::gfx::ImageUsage::Sampled | coopa::gfx::ImageUsage::TransferDst);

    vert_shader_    = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
    march_shader_   = std::make_unique<pipeline::Shader>(device, march_spv, ShaderStage::Fragment);
    resolve_shader_ = std::make_unique<pipeline::Shader>(device, resolve_spv, ShaderStage::Fragment);

    // Set 2 of the march: the three-binding G-buffer shape every other consumer uses, plus
    // the scene depth buffer the march compares its ray against (binding 3).
    gbuf_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, ShaderStage::Fragment)
            .combined_sampler(1, ShaderStage::Fragment)
            .combined_sampler(2, ShaderStage::Fragment)
            .combined_sampler(3, ShaderStage::Fragment)
            .build(device));
    // Set 0 of the resolve: current, history, depth, shared count (ssr_resolve.frag's shape).
    resolve_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, ShaderStage::Fragment)
            .combined_sampler(1, ShaderStage::Fragment)
            .combined_sampler(2, ShaderStage::Fragment)
            .combined_sampler(3, ShaderStage::Fragment)
            .build(device));

    pipeline::DescriptorPoolBuilder pool_builder;
    // Two resolve sets: one per parity of TemporalHistoryPass's ping-pong count buffer,
    // so the right one is selected per frame instead of a descriptor being rewritten.
    pool_builder.add_sets(*gbuf_layout_, 1).add_sets(*resolve_layout_, 2);
    desc_pool_ = std::make_unique<pipeline::DescriptorPool>(pool_builder.build(device));

    gbuf_set_ = std::make_unique<pipeline::DescriptorSet>(device, *desc_pool_, *gbuf_layout_);
    for (uint32_t c = 0; c < 2; ++c) {
        resolve_sets_[c] = std::make_unique<pipeline::DescriptorSet>(device, *desc_pool_, *resolve_layout_);
    }

    pipeline::PipelineDesc common;
    common.vertex      = VertexLayout::none();
    common.raster.cull = CullMode::None;
    common.depth.test  = false;
    common.depth.write = false;

    pipeline::PipelineDesc march_desc = common;
    march_desc.shaders            = {vert_shader_.get(), march_shader_.get()};
    march_desc.descriptor_layouts = {&camera_layout, &light_layout, gbuf_layout_.get()};
    march_pipeline_ = std::make_unique<pipeline::Pipeline>(
        device, march_target_->render_pass_object(), march_desc);

    pipeline::PipelineDesc resolve_desc = common;
    resolve_desc.shaders            = {vert_shader_.get(), resolve_shader_.get()};
    resolve_desc.descriptor_layouts = {resolve_layout_.get()};
    resolve_desc.push_constants     = {
        {ShaderStage::Fragment, 0,
         sizeof(coopa::gfx::engine::passes::SsrPass::ResolvePushConstants)}};
    resolve_pipeline_ = std::make_unique<pipeline::Pipeline>(
        device, resolved_target_->render_pass_object(), resolve_desc);
}

void ContactShadowPass::update_descriptors(coopa::gfx::TextureView g0, coopa::gfx::TextureView g1,
                        coopa::gfx::TextureView g2,
                        coopa::gfx::TextureView depth,
                        coopa::gfx::TextureView count0, coopa::gfx::TextureView count1,
                        const coopa::gfx::engine::util::Sampler& count_sampler)
{
    // NEAREST. The march fetches the G-buffer at arbitrary UVs along its ray, where bilinear
    // filtering blends world positions across silhouette edges into points that exist on no
    // real surface -- the same rule SsrPass keeps a dedicated nearest sampler for. The body
    // uses texelFetch for those reads so the sampler is moot there, but the binding should
    // not imply that blending this data is ever acceptable.
    gbuf_set_->bind_image(0, g0, *nearest_sampler_);
    gbuf_set_->bind_image(1, g1, *nearest_sampler_);
    gbuf_set_->bind_image(2, g2, *nearest_sampler_);
    // The march's per-step occluder test: one depth texel per step (D32_SFLOAT, NEAREST).
    gbuf_set_->bind_image(3, depth, *nearest_sampler_);

    for (uint32_t c = 0; c < 2; ++c) {
        resolve_sets_[c]->bind_image(0, march_target_->color_view_typed(), *nearest_sampler_);
        // LINEAR: the history is read at a reprojected, non-texel-aligned UV.
        resolve_sets_[c]->bind_image(1, history_image_->view_typed(), *linear_sampler_);
        // NEAREST is mandatory: D32_SFLOAT is not guaranteed to support linear filtering.
        resolve_sets_[c]->bind_image(2, depth, *nearest_sampler_);
        resolve_sets_[c]->bind_image(3, c == 0 ? count0 : count1, count_sampler);
    }
}

void ContactShadowPass::execute(coopa::gfx::command::CommandBuffer& cmd,
             const coopa::gfx::pipeline::DescriptorSet& camera_set,
             const coopa::gfx::pipeline::DescriptorSet& light_set,
             const Params& params)
{
    // On the first frame the history image has never been written and sits in UNDEFINED;
    // transition it before it is bound as a sampled image. The resolve does not read it in
    // that case (history_valid = 0), but the descriptor still needs a valid layout at draw
    // time regardless of the runtime branch -- the same one-time transition SsrPass and
    // SsaoPass both perform.
    if (!history_initialized_) {
        cmd.barrier(*history_image_, coopa::gfx::ImageBarrier{
            coopa::gfx::TextureUsage::Undefined, coopa::gfx::TextureUsage::ShaderRead});
    }

    // --- 1. March ---
    march_target_->begin(cmd);
    cmd.bind_pipeline(*march_pipeline_);
    cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
    cmd.set_scissor(0, 0, width_, height_);
    cmd.bind_descriptor_set(camera_set, 0);
    cmd.bind_descriptor_set(light_set, 1);
    cmd.bind_descriptor_set(*gbuf_set_, 2);
    cmd.draw(3);
    march_target_->end(cmd);

    // --- 2. Temporal resolve ---
    resolved_target_->begin(cmd);
    cmd.bind_pipeline(*resolve_pipeline_);
    cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
    cmd.set_scissor(0, 0, width_, height_);

    coopa::gfx::engine::passes::SsrPass::ResolvePushConstants rpc{};
    rpc.reproject     = params.reproject;
    rpc.resolution_x  = static_cast<float>(width_);
    rpc.resolution_y  = static_cast<float>(height_);
    rpc.max_accum     = static_cast<float>(params.temporal_frames);
    // Unreachable: this pass always binds a real count buffer. Carried only because the
    // shared push block declares it.
    rpc.blend_factor  = 0.0f;
    rpc.history_valid = (history_initialized_ && params.reproject_valid) ? 1 : 0;
    rpc.gamma         = params.temporal_gamma;
    rpc.frozen        = params.frozen ? 1 : 0;
    cmd.push_constants(coopa::gfx::ShaderStage::Fragment, rpc);
    cmd.bind_descriptor_set(*resolve_sets_[params.count_parity & 1], 0);
    cmd.draw(3);
    resolved_target_->end(cmd);

    copy_history_(cmd);
    history_initialized_ = true;
}

void ContactShadowPass::clear_output(coopa::gfx::command::CommandBuffer& cmd) {
    resolved_target_->begin(cmd, {{0.0f, 0.0f, 0.0f, 0.0f}});
    resolved_target_->end(cmd);
    history_initialized_ = false;
}

void ContactShadowPass::copy_history_(coopa::gfx::command::CommandBuffer& cmd) {
    using coopa::gfx::ImageBarrier;
    using coopa::gfx::TextureUsage;

    const coopa::gfx::ImageHandle src = resolved_target_->color_image_handle_typed();
    const coopa::gfx::ImageHandle dst = history_image_->handle_typed();

    // One vkCmdPipelineBarrier for both endpoints, as the hand-written pair recorded: splitting
    // it can split a tile encoder and move where GpuProfiler attributes the copy.
    //
    // No explicit access masks: each image has had exactly one role since the last barrier, so
    // the layouts alone derive the same masks the raw code spelled out.
    cmd.barriers({
        {src, ImageBarrier{TextureUsage::ShaderRead, TextureUsage::TransferSrc}},
        {dst, ImageBarrier{TextureUsage::ShaderRead, TextureUsage::TransferDst}},
    });

    const coopa::gfx::ImageCopy region = coopa::gfx::ImageCopy::whole(width_, height_);
    cmd.copy_image(src, dst, coopa::gfx::ImageAspect::Color, &region, 1);

    cmd.barriers({
        {src, ImageBarrier{TextureUsage::TransferSrc, TextureUsage::ShaderRead}},
        {dst, ImageBarrier{TextureUsage::TransferDst, TextureUsage::ShaderRead}},
    });
}

} // namespace passes
} // namespace render
} // namespace toy
