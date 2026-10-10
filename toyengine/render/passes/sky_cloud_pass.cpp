#include <toyengine/render/passes/sky_cloud_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

SkyCloudPass::SkyCloudPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
             uint32_t render_width, uint32_t render_height, uint32_t frames_in_flight,
             const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
             const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
             const std::string& vert_spv, const std::string& clouds_spv, const std::string& resolve_spv,
             const std::string& copy_spv, const std::string& weather_spv, const std::string& shape_spv,
             const std::string& detail_spv, const std::string& mip2d_spv, const std::string& mip3d_spv)
    : frames_(std::max(frames_in_flight, 1u)),
      linear_(coopa::gfx::engine::util::Sampler::linear(device)),
      nearest_(coopa::gfx::engine::util::Sampler::nearest(device)),
      noise_sampler_(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f, VK_SAMPLER_MIPMAP_MODE_LINEAR),
      gens_(device, mip2d_spv, mip3d_spv),
      weather_gen_(device, weather_spv, {&gens_.gen_layout()}, {{coopa::gfx::ShaderStage::Compute, 0, 4}}),
      shape_gen_(device, shape_spv, {&gens_.gen_layout()}, {{coopa::gfx::ShaderStage::Compute, 0, 4}}),
      detail_gen_(device, detail_spv, {&gens_.gen_layout()}, {{coopa::gfx::ShaderStage::Compute, 0, 4}}),
      weather_(device, allocator, gens_, kWeatherSize, false),
      shape_(device, allocator, gens_, kShapeSize, true),
      detail_(device, allocator, gens_, kDetailSize, true),
      trace_(device, allocator, half_(half_(render_width)), half_(half_(render_height)),
             coopa::gfx::Format::RGBA32_Sfloat, coopa::gfx::engine::targets::kColorOnly),
      output_(device, allocator, half_(render_width), half_(render_height), coopa::gfx::Format::RGBA32_Sfloat,
              coopa::gfx::engine::targets::kColorOnly),
      history_(device, allocator, half_(render_width), half_(render_height), coopa::gfx::Format::RGBA32_Sfloat,
               coopa::gfx::engine::targets::kColorOnly),
      trace_stage_(device, trace_.render_pass_object(), describe_trace_(vert_spv, clouds_spv, camera_layout, light_layout, frames_)),
      resolve_stage_(device, output_.render_pass_object(), describe_resolve_(vert_spv, resolve_spv, frames_)),
      copy_stage_(device, history_.render_pass_object(), describe_copy_(vert_spv, copy_spv)) {
    for (uint32_t i = 0; i < frames_; ++i) {
        ubos_.push_back(std::make_unique<coopa::gfx::memory::Buffer>(
            coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(FrameData))));
    }
}

void SkyCloudPass::set_inputs(coopa::gfx::TextureView transmittance, coopa::gfx::TextureView g_normal,
                coopa::gfx::TextureView g_position) {
    for (uint32_t i = 0; i < frames_; ++i) {
        auto& t = trace_stage_.set(0, i);
        t.bind_image(0, transmittance, linear_);
        t.bind_image(1, weather_.view(), noise_sampler_.handle());
        t.bind_image(2, g_normal, nearest_);
        t.bind_image(3, shape_.view(), noise_sampler_.handle());
        t.bind_image(4, detail_.view(), noise_sampler_.handle());
        t.bind_buffer(5, *ubos_[i]);
        t.bind_image(6, g_position, nearest_);
        auto& r = resolve_stage_.set(0, i);
        r.bind_image(0, trace_.color_view_typed(), nearest_);
        r.bind_image(1, history_.color_view_typed(), nearest_);
        r.bind_image(2, g_normal, nearest_);
        r.bind_buffer(3, *ubos_[i]);
    }
    copy_stage_.set(0).bind_image(0, output_.color_view_typed(), nearest_);
}

void SkyCloudPass::ensure_noise(coopa::gfx::command::CommandBuffer& cmd) {
    if (noise_ready_) return;
    weather_.generate(cmd, weather_gen_, gens_);
    shape_.generate(cmd, shape_gen_, gens_);
    detail_.generate(cmd, detail_gen_, gens_);
    noise_ready_ = true;
}

void SkyCloudPass::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, -1.0f}};
    trace_.begin(cmd, clear);
    trace_.end(cmd);
    output_.begin(cmd, clear);
    output_.end(cmd);
    history_.begin(cmd, clear);
    history_.end(cmd);
    initialized_ = true;
}

uint32_t SkyCloudPass::scaled_extent(uint32_t full, float scale) {
    return std::clamp(static_cast<uint32_t>(std::floor(static_cast<float>(full) * scale + 0.5f)), 1u, full);
}

void SkyCloudPass::execute(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot,
             const coopa::gfx::pipeline::DescriptorSet& camera_set,
             const coopa::gfx::pipeline::DescriptorSet& light_set, const Params& p, float scale,
             bool trace) {
    ensure_noise(cmd);
    const uint32_t slot = frame_slot % frames_;
    const uint32_t rw = scaled_extent(output_.width(), scale), rh = scaled_extent(output_.height(), scale);
    // The traced pixel of each 2x2 block, cycling in an order that spreads consecutive
    // frames' samples apart (a 2x2 Bayer matrix).
    static constexpr int kOrder[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const int* o = kOrder[p.frame & 3u];

    FrameData d;
    d.inv_view_proj = glm::inverse(p.view_proj);
    d.prev_view_proj = p.prev_view_proj;
    const float cs = std::max(p.scale, 1e-4f);
    d.slab = glm::vec4(p.slab.x / cs, p.slab.y / cs, p.slab.z, p.slab.w);
    d.wind = glm::vec4(p.wind_offset, p.time, static_cast<float>(p.frame & 0xFFFFu));
    d.light_dir = glm::vec4(p.light_dir, static_cast<float>(p.view_steps));
    d.light_color = glm::vec4(p.light_color, static_cast<float>(p.shadow_steps));
    d.trace = glm::vec4(static_cast<float>(o[0]), static_cast<float>(o[1]), static_cast<float>(rw), static_cast<float>(rh));
    // A region pixel's angular size: the vertical field of view over the region's rows.
    const float pix_angle = 2.0f / (std::max(p.proj_y, 1e-3f) * static_cast<float>(rh));
    const bool valid = p.history_valid && last_extent_ == glm::uvec2(rw, rh);
    d.history = glm::vec4(p.drift, valid ? 1.0f : 0.0f, pix_angle);
    d.camera = glm::vec4(p.camera_pos, p.light_change);
    d.look = glm::vec4(cs, std::clamp(p.fade, 0.0f, 1.0f), p.atmosphere_lut ? 1.0f : 0.0f, 0.0f);
    ubos_[slot]->upload(&d, sizeof(d));
    if (!trace) { last_extent_ = glm::uvec2(0u); return; }
    last_extent_ = glm::uvec2(rw, rh);

    trace_.begin(cmd);
    trace_stage_.bind(cmd, (rw + 1) / 2, (rh + 1) / 2, slot);
    cmd.bind_descriptor_set(camera_set, 0);
    cmd.bind_descriptor_set(light_set, 1);
    trace_stage_.draw(cmd);
    trace_.end(cmd);

    output_.begin(cmd);
    resolve_stage_.bind(cmd, rw, rh, slot);
    resolve_stage_.draw(cmd);
    output_.end(cmd);

    history_.begin(cmd);
    copy_stage_.bind(cmd, rw, rh);
    copy_stage_.draw(cmd);
    history_.end(cmd);
}

coopa::gfx::engine::passes::FullscreenStageDesc SkyCloudPass::describe_trace_(
        const std::string& vert_spv, const std::string& frag_spv,
        const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
        const coopa::gfx::pipeline::DescriptorSetLayout& light_layout, uint32_t frames) {
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
         {4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
         {5, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1},
         {6, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
    };
    d.instances = frames;
    return d;
}

coopa::gfx::engine::passes::FullscreenStageDesc SkyCloudPass::describe_resolve_(
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
    d.instances = frames;
    return d;
}

coopa::gfx::engine::passes::FullscreenStageDesc SkyCloudPass::describe_copy_(const std::string& vert_spv,
                                                                      const std::string& frag_spv) {
    using coopa::gfx::DescriptorType;
    using coopa::gfx::ShaderStage;
    coopa::gfx::engine::passes::FullscreenStageDesc d;
    d.vert_spv = vert_spv;
    d.frag_spv = frag_spv;
    d.owned_sets = {{{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}}};
    return d;
}

} // namespace passes
} // namespace render
} // namespace toy
