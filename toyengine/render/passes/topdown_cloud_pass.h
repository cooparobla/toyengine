/**
 * @file topdown_cloud_pass.h
 * @brief Topdown mode's toon cloud layer and its ground shadows (render topdown_mode): one
 *        fullscreen draw over the HDR scene, after the translucent pass (so the clouds also
 *        cover water and other BLEND surfaces below them), blended premultiplied.
 *
 * See assets/shaders/topdown_clouds.frag. Reads the G-buffer's normals (sky test) and world
 * positions (the shadow lookup and the clouds' occlusion by tall geometry), and the physical
 * sky's weather map and shape volume (SkyCloudPass, baked on first use) for the cloud field.
 * Lit from the camera and light sets (sets 0 and 1) -- the scene's sun and sky colours -- so it
 * works with either sky model. Built against the translucent pass's render pass.
 */

#ifndef TOYENGINE_RENDER_PASSES_TOPDOWN_CLOUD_PASS_H
#define TOYENGINE_RENDER_PASSES_TOPDOWN_CLOUD_PASS_H

#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/texture_view.h>

#include <toyengine/render/sky_state.h>

namespace toy {
namespace render {
namespace passes {

class TopdownCloudPass {
public:
    /// topdown_clouds.frag's push constants.
    struct alignas(16) Params {
        glm::mat4 inv_view_proj{1.0f};
        glm::vec4 layer{60.0f, 30.0f, 8.0f, 0.92f};   ///< height, puff size, thickness, opacity
        glm::vec4 look{15.0f, 60.0f, 0.45f, 3.0f};    ///< fade start, fade end, shadow strength, light bands
        glm::vec4 wind{0.0f, 0.0f, 0.4f, 0.5f};       ///< drift xy, coverage, outline
        glm::vec4 extra{0.0f};                        ///< x time
    };
    static_assert(sizeof(Params) == 128, "TopdownCloudPass::Params must fill exactly the 128-byte push range");

    static Params params_of(const TopdownCloudState& st, const glm::mat4& inv_view_proj) {
        Params p;
        p.inv_view_proj = inv_view_proj;
        p.layer = glm::vec4(st.height, st.size, st.thickness, st.opacity);
        p.look = glm::vec4(st.fade_start, st.fade_end, st.shadow_strength, st.light_bands);
        p.wind = glm::vec4(st.offset, st.coverage, st.outline);
        p.extra = glm::vec4(st.time, 0.0f, 0.0f, 0.0f);
        return p;
    }

    TopdownCloudPass(coopa::gfx::core::Device& device, VkRenderPass render_pass,
                     const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                     const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                     const std::string& vert_spv, const std::string& frag_spv)
        : nearest_(coopa::gfx::engine::util::Sampler::nearest(device)),
          stage_(device, coopa::gfx::detail::RawRenderPass{render_pass},
                 describe_(camera_layout, light_layout, vert_spv, frag_spv)) {}

    TopdownCloudPass(const TopdownCloudPass&) = delete;
    TopdownCloudPass& operator=(const TopdownCloudPass&) = delete;

    /** @brief Binds the G-buffer and the cloud noise (once, at construction). */
    void set_inputs(coopa::gfx::TextureView g_normal, coopa::gfx::TextureView g_position,
                    VkImageView weather, VkImageView shape, VkSampler noise_sampler) {
        stage_.set(0).bind_image(0, g_normal, nearest_);
        stage_.set(0).bind_image(1, g_position, nearest_);
        stage_.set(0).bind_image(2, weather, noise_sampler);
        stage_.set(0).bind_image(3, shape, noise_sampler);
    }

    /// @brief Records the draw. Must be inside the render pass given at construction.
    void draw(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& camera_set,
              const coopa::gfx::pipeline::DescriptorSet& light_set, const Params& p,
              uint32_t width, uint32_t height) const {
        stage_.bind(cmd, width, height);
        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(light_set, 1);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, p);
        stage_.draw(cmd);
    }

private:
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_(
            const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
            const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
            const std::string& vert_spv, const std::string& frag_spv) {
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
             {3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
        };
        d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(Params))}};
        d.blend = coopa::gfx::pipeline::BlendMode::PremultipliedAlpha;
        return d;
    }

    coopa::gfx::engine::util::Sampler           nearest_;
    coopa::gfx::engine::passes::FullscreenStage stage_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_TOPDOWN_CLOUD_PASS_H
