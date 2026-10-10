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
    const VkImage src = scene_target_.color_image_object()->handle();
    const VkImage dst = source_copy_.handle();
    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                       VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = from;
        b.newLayout           = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = img;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = sa;
        b.dstAccessMask       = da;
        vkCmdPipelineBarrier(cmd.handle(), ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const VkPipelineStageFlags producers = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            producers, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // The copy is overwritten whole, so its old contents (last frame's) are discarded; the
    // fragment-shader source stage orders this after last frame's gather read it.
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent         = {width_, height_, 1};
    vkCmdCopyImage(cmd.handle(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Back to SHADER_READ_ONLY, ordered before the gather's render pass overwrites it (WAR).
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

} // namespace passes
} // namespace render
} // namespace toy
