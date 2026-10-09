/**
 * @file sky_cloud_pass.h
 * @brief The physical sky's cloud layer: a tiling 2D shape map and a 64^3 tiling 3D noise packed
 *        into a 2D atlas (both 512 x 512, rendered once) and a
 *        half-resolution raymarch of the layer each frame clouds are on.
 *
 * See assets/shaders/sky_clouds.frag for the march and sky_cloud_noise.frag for the map. The
 * march reads the camera and light sets (bound by the caller, sets 0 and 1), the atmosphere's
 * transmittance and sky-view tables (SkyAtmospherePass) and the G-buffer normals (to skip blocks
 * that hold no sky). The lighting pass upsamples the result -- rgb in-scatter, a transmittance --
 * over its physical sky.
 *
 * Both targets exist from construction and are bound once, so `clouds` switches live; while it is
 * off nothing is recorded beyond initialize()'s one-time clear.
 */

#ifndef TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H
#define TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class SkyCloudPass
 * @brief See the file doc.
 */
class SkyCloudPass {
public:
    static constexpr uint32_t kNoiseSize = 512;

    /// sky_clouds.frag's push constants.
    struct alignas(16) Params {
        glm::mat4 inv_view_proj{1.0f};
        glm::vec4 slab{1500.0f, 1200.0f, 0.4f, 1.0f};   ///< base (m), thickness (m), coverage, density
        glm::vec4 wind{0.0f, 0.0f, 0.0f, -1.0f};        ///< xy offset (m), z time (s), w jitter frame (< 0 none)
        glm::vec4 light_dir{0.0f, 0.0f, 1.0f, 24.0f};   ///< xyz to the light, w view steps
        glm::vec4 light_color{0.0f, 0.0f, 0.0f, 4.0f};  ///< rgb illuminance, w shadow steps
    };
    static_assert(sizeof(Params) == 128, "SkyCloudPass::Params must fill exactly the 128-byte push range");

    SkyCloudPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                 uint32_t render_width, uint32_t render_height,
                 const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                 const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                 const std::string& vert_spv, const std::string& noise_spv, const std::string& noise3d_spv,
                 const std::string& clouds_spv)
        : linear_(coopa::gfx::engine::util::Sampler::linear(device)),
          nearest_(coopa::gfx::engine::util::Sampler::nearest(device)),
          repeat_(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT),
          noise_(device, allocator, kNoiseSize, kNoiseSize, coopa::gfx::Format::RGBA8_Unorm,
                 coopa::gfx::engine::targets::kColorOnly),
          noise3d_(device, allocator, kNoiseSize, kNoiseSize, coopa::gfx::Format::RGBA8_Unorm,
                   coopa::gfx::engine::targets::kColorOnly),
          clouds_(device, allocator, half_(render_width), half_(render_height), coopa::gfx::Format::RGBA16_Sfloat,
                  coopa::gfx::engine::targets::kColorOnly),
          noise_stage_(device, noise_.render_pass_object(), describe_noise_(vert_spv, noise_spv)),
          noise3d_stage_(device, noise3d_.render_pass_object(), describe_noise_(vert_spv, noise3d_spv)),
          clouds_stage_(device, clouds_.render_pass_object(), describe_clouds_(vert_spv, clouds_spv, camera_layout, light_layout)) {}

    SkyCloudPass(const SkyCloudPass&) = delete;
    SkyCloudPass& operator=(const SkyCloudPass&) = delete;

    /** @brief Binds the atmosphere tables and the G-buffer normals (once, at construction). */
    void set_inputs(coopa::gfx::TextureView transmittance, coopa::gfx::TextureView sky_view,
                    coopa::gfx::TextureView g_normal) {
        clouds_stage_.set(0).bind_image(0, transmittance, linear_);
        clouds_stage_.set(0).bind_image(1, sky_view, linear_);
        clouds_stage_.set(0).bind_image(2, noise_.color_view_typed(), repeat_);
        clouds_stage_.set(0).bind_image(3, g_normal, nearest_);
        clouds_stage_.set(0).bind_image(4, noise3d_.color_view_typed(), linear_);
    }

    coopa::gfx::TextureView output_view() const { return clouds_.color_view_typed(); }
    const coopa::gfx::engine::util::Sampler& sampler() const { return linear_; }
    uint32_t width() const { return clouds_.width(); }
    uint32_t height() const { return clouds_.height(); }

    /** @brief Clears the output to "no cloud" once, so the bound image is laid out. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd) {
        if (initialized_) return;
        clouds_.begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
        clouds_.end(cmd);
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
     * @brief Marches the layer into the top-left `scale` fraction (per axis) of the
     *        half-resolution target (rendering the noise maps first if they have not been yet).
     *        The camera and light sets go at sets 0 and 1.
     */
    void execute(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const coopa::gfx::pipeline::DescriptorSet& light_set, const Params& params, float scale = 1.0f) {
        if (!noise_ready_) {
            noise_.begin(cmd);
            noise_stage_.bind(cmd, kNoiseSize, kNoiseSize);
            noise_stage_.draw(cmd);
            noise_.end(cmd);
            noise3d_.begin(cmd);
            noise3d_stage_.bind(cmd, kNoiseSize, kNoiseSize);
            noise3d_stage_.draw(cmd);
            noise3d_.end(cmd);
            noise_ready_ = true;
        }
        clouds_.begin(cmd);
        clouds_stage_.bind(cmd, scaled_extent(clouds_.width(), scale), scaled_extent(clouds_.height(), scale));
        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(light_set, 1);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, params);
        clouds_stage_.draw(cmd);
        clouds_.end(cmd);
    }

private:
    static uint32_t half_(uint32_t v) { return std::max(1u, (v + 1u) / 2u); }

    static coopa::gfx::engine::passes::FullscreenStageDesc describe_noise_(const std::string& vert_spv, const std::string& frag_spv) {
        coopa::gfx::engine::passes::FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        return d;
    }
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_clouds_(
            const std::string& vert_spv, const std::string& frag_spv,
            const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
            const coopa::gfx::pipeline::DescriptorSetLayout& light_layout) {
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
             {4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
        };
        d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(Params))}};
        return d;
    }

    coopa::gfx::engine::util::Sampler            linear_;
    coopa::gfx::engine::util::Sampler            nearest_;
    coopa::gfx::engine::util::Sampler            repeat_;
    coopa::gfx::engine::targets::OffscreenTarget noise_;
    coopa::gfx::engine::targets::OffscreenTarget noise3d_;   ///< 64^3 noise as 8 x 8 slices (sky_cloud_noise3d.frag)
    coopa::gfx::engine::targets::OffscreenTarget clouds_;
    coopa::gfx::engine::passes::FullscreenStage  noise_stage_;
    coopa::gfx::engine::passes::FullscreenStage  noise3d_stage_;
    coopa::gfx::engine::passes::FullscreenStage  clouds_stage_;
    bool noise_ready_ = false;
    bool initialized_ = false;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_SKY_CLOUD_PASS_H
