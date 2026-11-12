#include <toyengine/render/passes/motion_blur_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

MotionBlurPass::MotionBlurPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
               uint32_t width, uint32_t height,
               coopa::gfx::engine::targets::OffscreenTarget& scene_target,
               coopa::gfx::TextureView velocity,
               const coopa::gfx::pipeline::ShaderLibrary& shaders)
    : width_(width), height_(height),
      tile_(std::max(4u, static_cast<uint32_t>(std::lround(kTileAt1080 * static_cast<float>(height) / 1080.0f)))),
      tiles_w_((width + tile_ - 1) / tile_), tiles_h_((height + tile_ - 1) / tile_),
      scene_target_(scene_target),
      nearest_(coopa::gfx::engine::util::Sampler::nearest(device)),
      tile_target_(device, allocator, tiles_w_, tiles_h_, coopa::gfx::Format::RG16_Sfloat,
                   coopa::gfx::engine::targets::kColorOnly),
      neighbor_target_(device, allocator, tiles_w_, tiles_h_, coopa::gfx::Format::RG16_Sfloat,
                       coopa::gfx::engine::targets::kColorOnly),
      source_copy_(device, allocator, width, height, coopa::gfx::Format::RGBA16_Sfloat,
                   coopa::gfx::ImageUsage::Sampled | coopa::gfx::ImageUsage::TransferDst),
      tile_stage_(device, tile_target_.render_pass_object(),
                  describe_(shaders("fullscreen.vert"), shaders("motion_blur_tile_max.frag"), 1, sizeof(StagePush))),
      neighbor_stage_(device, neighbor_target_.render_pass_object(),
                      describe_(shaders("fullscreen.vert"), shaders("motion_blur_neighbor_max.frag"), 1, sizeof(NeighborPush))),
      gather_stage_(device, scene_target.render_pass_object(),
                    describe_(shaders("fullscreen.vert"), shaders("motion_blur_gather.frag"), 3, sizeof(StagePush))) {
    tile_stage_.set(0).bind_image(0, velocity, nearest_);
    neighbor_stage_.set(0).bind_image(0, tile_target_.color_image_object()->view_typed(), nearest_);
    gather_stage_.set(0).bind_image(0, source_copy_.view_typed(), nearest_);
    gather_stage_.set(0).bind_image(1, velocity, nearest_);
    gather_stage_.set(0).bind_image(2, neighbor_target_.color_image_object()->view_typed(), nearest_);
}

void MotionBlurPass::execute(coopa::gfx::command::CommandBuffer& cmd, const Params& params) {
    StagePush sp{};
    sp.sky_reproject = params.sky_reproject;
    sp.extent_scale  = glm::vec4(static_cast<float>(width_), static_cast<float>(height_),
                                 0.5f * std::max(params.shutter, 0.0f), std::max(params.max_radius, 0.0f));
    sp.info          = glm::ivec4(static_cast<int>(tile_), params.sky_valid ? 1 : 0,
                                  std::clamp(params.samples, 2, 64), static_cast<int>(params.noise_frame & 0xFFu));

    tile_target_.begin(cmd);
    tile_stage_.draw(cmd, tiles_w_, tiles_h_, coopa::gfx::ShaderStage::Fragment, sp);
    tile_target_.end(cmd);

    NeighborPush np{};
    const int reach = static_cast<int>(std::ceil(sp.extent_scale.w / static_cast<float>(tile_)));
    np.info = glm::ivec4(std::clamp(reach, 1, kMaxReach), static_cast<int>(tiles_w_), static_cast<int>(tiles_h_), 0);
    neighbor_target_.begin(cmd);
    neighbor_stage_.draw(cmd, tiles_w_, tiles_h_, coopa::gfx::ShaderStage::Fragment, np);
    neighbor_target_.end(cmd);

    copy_scene_(cmd);

    scene_target_.begin(cmd);
    gather_stage_.draw(cmd, width_, height_, coopa::gfx::ShaderStage::Fragment, sp);
    scene_target_.end(cmd);
}

coopa::gfx::engine::passes::FullscreenStageDesc MotionBlurPass::describe_(const std::string& vert, const std::string& frag,
                                                                 uint32_t images, size_t push_size) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert;
    d.frag_spv = frag;
    std::vector<coopa::gfx::pipeline::DescriptorBinding> set;
    for (uint32_t i = 0; i < images; ++i) set.push_back({i, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1});
    d.owned_sets = {set};
    d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(push_size)}};
    return d;
}

void MotionBlurPass::copy_scene_(coopa::gfx::command::CommandBuffer& cmd) {
    using coopa::gfx::ImageAccess;
    using coopa::gfx::ImageBarrier;
    using coopa::gfx::ImageCopy;
    using coopa::gfx::TextureUsage;

    const coopa::gfx::ImageHandle src = scene_target_.color_image_handle_typed();
    const coopa::gfx::ImageHandle dst = source_copy_.handle_typed();

    // Both endpoints move together, in one barrier, so the recorded command stream matches what
    // the hand-written version produced -- see CommandBuffer::barriers().
    //
    // The masks are stated rather than derived because neither side is a single-role image.
    // `src` was written as a colour attachment, sampled, AND copied since the last barrier, so
    // no one layout describes its producers. `dst` is overwritten whole, so its old contents
    // (last frame's) are discarded -- but last frame's gather READ them, so the Undefined
    // source layout must still wait on the fragment shader. That is the write-after-read case
    // a derived mask cannot express, since Undefined implies no access.
    cmd.barriers({
        {src, ImageBarrier{TextureUsage::ShaderRead, TextureUsage::TransferSrc,
                           coopa::gfx::ImageAspect::Color, {},
                           ImageAccess::ColorAttachmentWrite | ImageAccess::TransferWrite
                               | ImageAccess::FragmentShaderRead,
                           ImageAccess::TransferRead}},
        {dst, ImageBarrier{TextureUsage::Undefined, TextureUsage::TransferDst,
                           coopa::gfx::ImageAspect::Color, {},
                           ImageAccess::FragmentShaderRead,
                           ImageAccess::TransferWrite}},
    });

    const ImageCopy region = ImageCopy::whole(width_, height_);
    cmd.copy_image(src, dst, coopa::gfx::ImageAspect::Color, &region, 1);

    // Back to ShaderRead for the gather. `src` is ordered before the gather's render pass
    // overwrites it as a colour attachment (a write-after-read on the copy's own read).
    cmd.barriers({
        {src, ImageBarrier{TextureUsage::TransferSrc, TextureUsage::ShaderRead,
                           coopa::gfx::ImageAspect::Color, {},
                           ImageAccess::TransferRead,
                           ImageAccess::ColorAttachmentWrite | ImageAccess::FragmentShaderRead}},
        {dst, ImageBarrier{TextureUsage::TransferDst, TextureUsage::ShaderRead,
                           coopa::gfx::ImageAspect::Color, {},
                           ImageAccess::TransferWrite,
                           ImageAccess::FragmentShaderRead}},
    });
}

} // namespace passes
} // namespace render
} // namespace toy
