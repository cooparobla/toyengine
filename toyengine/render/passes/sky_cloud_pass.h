/**
 * @file sky_cloud_pass.h
 * @brief The physical sky's cloud layer: a checkerboarded raymarch with its own temporal
 *        reconstruction (the Unreal / Horizon Zero Dawn scheme), over mipmapped weather, shape and
 *        detail noise baked once by compute.
 *
 * Per frame, while clouds are on:
 *   1. trace (sky_clouds.frag): one pixel of every 2x2 block of the cloud region -- the
 *      half-resolution target's top-left `scale` fraction -- marched into a quarter-size target;
 *      the traced pixel cycles through the block over four frames.
 *   2. resolve (sky_cloud_resolve.frag): the full region, reprojecting last frame's result along
 *      each pixel's ray at the clouds' distance (and the wind's drift), blending in the fresh
 *      samples, clamped to their neighbourhood so nothing ghosts. Writes output().
 *   3. copy: output() into the history the next resolve reads (output() itself stays bound to
 *      the lighting pass for good -- rebinding per frame would race the frame in flight).
 * The lighting pass's sky branch upsamples output() (sky_physical.glsl). The trace's texels pack
 * the transmittance with the clouds' distance (sky_cloud_common.glsl), so that target is RGBA32F;
 * the reconstructed layer and its history are plain RGBA16F (rgb, transmittance).
 *
 * Noise (CloudNoiseTexture, generated on first use): a 512^2 weather map (coverage field, cloud
 * type, variation), a 128^3 Perlin-Worley base-shape volume and a 64^3 Worley detail volume, all
 * with full mip chains so the march reads each at its sample's screen footprint.
 *
 * Per-frame parameters go in a uniform buffer (FrameData, one per frame in flight); every stage
 * owns one descriptor set instance per frame slot, bound to that slot's buffer.
 *
 * All targets exist from construction and are bound once, so `clouds` switches live; while it is
 * off nothing is recorded beyond initialize()'s one-time clear.
 */

#ifndef TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H
#define TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

#include <toyengine/render/passes/cloud_noise_texture.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class SkyCloudPass
 * @brief See the file doc.
 */
class SkyCloudPass {
public:
    static constexpr uint32_t kWeatherSize = 512;
    static constexpr uint32_t kShapeSize = 128;
    static constexpr uint32_t kDetailSize = 64;

    /// sky_cloud_common.glsl's CloudFrame block (std140).
    struct alignas(16) FrameData {
        glm::mat4 inv_view_proj{1.0f};
        glm::mat4 prev_view_proj{1.0f};
        glm::vec4 slab{1500.0f, 1200.0f, 0.4f, 1.0f};   ///< base (m), thickness (m), coverage, density
        glm::vec4 wind{0.0f};                           ///< xy offset (m), z time (s), w frame index
        glm::vec4 light_dir{0.0f, 0.0f, 1.0f, 32.0f};   ///< xyz to the light, w view steps
        glm::vec4 light_color{0.0f, 0.0f, 0.0f, 5.0f};  ///< rgb illuminance, w shadow steps
        glm::vec4 trace{0.0f};                          ///< xy traced pixel, zw region (px) -- filled by execute()
        glm::vec4 history{0.0f};                        ///< xy drift (m), z history valid, w radians per region px
        glm::vec4 camera{0.0f};                         ///< xyz camera position, w this frame's relative lighting change
    };
    static_assert(sizeof(FrameData) == 240, "SkyCloudPass::FrameData must match sky_cloud_common.glsl's CloudFrame");

    /// What the pipeline fills each frame (execute() derives the rest).
    struct Params {
        glm::mat4 view_proj{1.0f};        ///< unjittered proj * view, this frame
        glm::mat4 prev_view_proj{1.0f};   ///< unjittered proj * view, previous frame
        glm::vec3 camera_pos{0.0f};
        float     proj_y = 1.0f;          ///< proj[1][1] (cot of half the vertical fov)
        glm::vec4 slab{1500.0f, 1200.0f, 0.4f, 1.0f};
        glm::vec2 wind_offset{0.0f};
        glm::vec2 drift{0.0f};            ///< wind_offset's change since the previous frame (m)
        float     time = 0.0f;
        uint64_t  frame = 0;
        glm::vec3 light_dir{0.0f, 0.0f, 1.0f};
        glm::vec3 light_color{0.0f};
        int       view_steps = 32;
        int       shadow_steps = 5;
        bool      history_valid = false;  ///< false: a camera cut, or the first frame clouds are on
        float     light_change = 0.0f;    ///< relative change of the clouds' light / ambient or layer settings since last frame
    };

    SkyCloudPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
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
          output_(device, allocator, half_(render_width), half_(render_height), coopa::gfx::Format::RGBA16_Sfloat,
                  coopa::gfx::engine::targets::kColorOnly),
          history_(device, allocator, half_(render_width), half_(render_height), coopa::gfx::Format::RGBA16_Sfloat,
                   coopa::gfx::engine::targets::kColorOnly),
          trace_stage_(device, trace_.render_pass_object(), describe_trace_(vert_spv, clouds_spv, camera_layout, light_layout, frames_)),
          resolve_stage_(device, output_.render_pass_object(), describe_resolve_(vert_spv, resolve_spv, frames_)),
          copy_stage_(device, history_.render_pass_object(), describe_copy_(vert_spv, copy_spv)) {
        for (uint32_t i = 0; i < frames_; ++i) {
            ubos_.push_back(std::make_unique<coopa::gfx::memory::Buffer>(
                coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(FrameData))));
        }
    }

    SkyCloudPass(const SkyCloudPass&) = delete;
    SkyCloudPass& operator=(const SkyCloudPass&) = delete;

    /** @brief Binds the atmosphere's transmittance table and the G-buffer normals (once, at construction). */
    void set_inputs(coopa::gfx::TextureView transmittance, coopa::gfx::TextureView g_normal) {
        for (uint32_t i = 0; i < frames_; ++i) {
            auto& t = trace_stage_.set(0, i);
            t.bind_image(0, transmittance, linear_);
            t.bind_image(1, weather_.view(), noise_sampler_.handle());
            t.bind_image(2, g_normal, nearest_);
            t.bind_image(3, shape_.view(), noise_sampler_.handle());
            t.bind_image(4, detail_.view(), noise_sampler_.handle());
            t.bind_buffer(5, *ubos_[i]);
            auto& r = resolve_stage_.set(0, i);
            r.bind_image(0, trace_.color_view_typed(), nearest_);
            r.bind_image(1, history_.color_view_typed(), nearest_);
            r.bind_image(2, g_normal, nearest_);
            r.bind_buffer(3, *ubos_[i]);
        }
        copy_stage_.set(0).bind_image(0, output_.color_view_typed(), nearest_);
    }

    coopa::gfx::TextureView output_view() const { return output_.color_view_typed(); }
    /// The baked noise (valid once ensure_noise() has run), for the topdown cloud layer.
    VkImageView weather_view() const { return weather_.view(); }
    VkImageView shape_view() const { return shape_.view(); }
    VkSampler noise_sampler() const { return noise_sampler_.handle(); }

    /** @brief Bakes the noise textures if they have not been yet. Outside any render pass. */
    void ensure_noise(coopa::gfx::command::CommandBuffer& cmd) {
        if (noise_ready_) return;
        weather_.generate(cmd, weather_gen_, gens_);
        shape_.generate(cmd, shape_gen_, gens_);
        detail_.generate(cmd, detail_gen_, gens_);
        noise_ready_ = true;
    }
    /** @brief The sampler to bind output_view() with: nearest (the upsample texelFetches it). */
    const coopa::gfx::engine::util::Sampler& sampler() const { return nearest_; }
    uint32_t width() const { return output_.width(); }
    uint32_t height() const { return output_.height(); }

    /** @brief Clears every target to "no cloud" once, so the bound images are laid out. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd) {
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

    /**
     * @brief The size of the target's top-left region a march at `scale` (0..1] of the
     *        half-resolution target fills. sky_physical.glsl's upsample derives the same region
     *        from the scale with the same rounding.
     */
    static uint32_t scaled_extent(uint32_t full, float scale) {
        return std::clamp(static_cast<uint32_t>(std::floor(static_cast<float>(full) * scale + 0.5f)), 1u, full);
    }

    /**
     * @brief Traces, reconstructs and stores the history for this frame, over the top-left
     *        `scale` fraction (per axis) of the half-resolution target (baking the noise first if
     *        it has not been yet). The camera and light sets go at sets 0 and 1 of the trace.
     *        Outside any render pass.
     */
    void execute(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const coopa::gfx::pipeline::DescriptorSet& light_set, const Params& p, float scale = 1.0f) {
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
        d.slab = p.slab;
        d.wind = glm::vec4(p.wind_offset, p.time, static_cast<float>(p.frame & 0xFFFFu));
        d.light_dir = glm::vec4(p.light_dir, static_cast<float>(p.view_steps));
        d.light_color = glm::vec4(p.light_color, static_cast<float>(p.shadow_steps));
        d.trace = glm::vec4(static_cast<float>(o[0]), static_cast<float>(o[1]), static_cast<float>(rw), static_cast<float>(rh));
        // A region pixel's angular size: the vertical field of view over the region's rows.
        const float pix_angle = 2.0f / (std::max(p.proj_y, 1e-3f) * static_cast<float>(rh));
        const bool valid = p.history_valid && last_extent_ == glm::uvec2(rw, rh);
        d.history = glm::vec4(p.drift, valid ? 1.0f : 0.0f, pix_angle);
        d.camera = glm::vec4(p.camera_pos, p.light_change);
        ubos_[slot]->upload(&d, sizeof(d));
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

private:
    static uint32_t half_(uint32_t v) { return std::max(1u, (v + 1u) / 2u); }

    static coopa::gfx::engine::passes::FullscreenStageDesc describe_trace_(
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
             {5, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
        };
        d.instances = frames;
        return d;
    }
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_resolve_(
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
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_copy_(const std::string& vert_spv,
                                                                          const std::string& frag_spv) {
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        coopa::gfx::engine::passes::FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {{{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}}};
        return d;
    }

    uint32_t                                     frames_;
    coopa::gfx::engine::util::Sampler            linear_;
    coopa::gfx::engine::util::Sampler            nearest_;
    coopa::gfx::engine::util::Sampler            noise_sampler_;   ///< trilinear, repeating, full mip chain
    CloudNoiseGenerators                         gens_;
    coopa::gfx::pipeline::ComputePipeline        weather_gen_;
    coopa::gfx::pipeline::ComputePipeline        shape_gen_;
    coopa::gfx::pipeline::ComputePipeline        detail_gen_;
    CloudNoiseTexture                            weather_;
    CloudNoiseTexture                            shape_;
    CloudNoiseTexture                            detail_;
    coopa::gfx::engine::targets::OffscreenTarget trace_;     ///< quarter of the half-res target: one pixel per 2x2 block (packed, RGBA32F)
    coopa::gfx::engine::targets::OffscreenTarget output_;    ///< the reconstructed layer (bound to the lighting pass)
    coopa::gfx::engine::targets::OffscreenTarget history_;   ///< last frame's output_ (read by the resolve)
    coopa::gfx::engine::passes::FullscreenStage  trace_stage_;
    coopa::gfx::engine::passes::FullscreenStage  resolve_stage_;
    coopa::gfx::engine::passes::FullscreenStage  copy_stage_;
    std::vector<std::unique_ptr<coopa::gfx::memory::Buffer>> ubos_;   ///< FrameData, one per frame in flight
    glm::uvec2 last_extent_{0u};
    bool noise_ready_ = false;
    bool initialized_ = false;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H
