/**
 * @file sky_cloud_pass.h
 * @brief The volumetric cloud layer (render clouds, cloud_type: volumetric), with either sky
 *        model: a checkerboarded raymarch with its own temporal reconstruction (the Unreal /
 *        Horizon Zero Dawn scheme), over mipmapped weather, shape and detail noise baked once by
 *        compute. The noise also feeds the flat clouds and both cloud shadow maps.
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
 * The lighting pass upsamples output() over the sky (sky_physical.glsl), and CloudOverlayPass
 * composites it over geometry (cloud_composite.frag). Every cloud texel packs the transmittance
 * with the clouds' distance (sky_cloud_common.glsl), so all three targets are RGBA32F. The march
 * runs in cloud space (world / render cloud_scale) and stops at the scene's surfaces, so a small,
 * low layer lies between a topdown camera and the ground.
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

#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>

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
        glm::vec4 look{1.0f, 1.0f, 1.0f, 0.0f};         ///< x cloud scale, y camera fade, z light_color is above the atmosphere (1) / as is (0)
    };
    static_assert(sizeof(FrameData) == 256, "SkyCloudPass::FrameData must match sky_cloud_common.glsl's CloudFrame");

    /// What the pipeline fills each frame (execute() derives the rest).
    struct Params {
        glm::mat4 view_proj{1.0f};        ///< unjittered proj * view, this frame
        glm::mat4 prev_view_proj{1.0f};   ///< unjittered proj * view, previous frame
        glm::vec3 camera_pos{0.0f};
        float     proj_y = 1.0f;          ///< proj[1][1] (cot of half the vertical fov)
        glm::vec4 slab{1500.0f, 1200.0f, 0.4f, 1.0f};   ///< world metres: base, thickness; coverage, density
        float     scale = 1.0f;           ///< world metres per cloud-space metre (render cloud_scale)
        float     fade = 1.0f;            ///< camera fade, 0..1
        bool      atmosphere_lut = false; ///< light_color is above the atmosphere: colour it by the transmittance table
        glm::vec2 wind_offset{0.0f};      ///< cloud space, already wrapped to the noise's periods
        glm::vec2 drift{0.0f};            ///< the clouds' world movement since the previous frame (m)
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
                 const std::string& detail_spv, const std::string& mip2d_spv, const std::string& mip3d_spv);

    SkyCloudPass(const SkyCloudPass&) = delete;
    SkyCloudPass& operator=(const SkyCloudPass&) = delete;

    /** @brief Binds the atmosphere's transmittance table and the G-buffer normals and positions
     *         (once, at construction). */
    void set_inputs(coopa::gfx::TextureView transmittance, coopa::gfx::TextureView g_normal,
                    coopa::gfx::TextureView g_position);

    coopa::gfx::TextureView output_view() const { return output_.color_view_typed(); }
    /// The baked noise (valid once ensure_noise() has run), for the flat clouds and the shadow maps.
    VkImageView weather_view() const { return weather_.view(); }
    VkImageView shape_view() const { return shape_.view(); }
    VkImageView detail_view() const { return detail_.view(); }
    VkSampler noise_sampler() const { return noise_sampler_.handle(); }
    /// The per-frame-slot FrameData buffer (filled by execute()), for the volumetric shadow map.
    const coopa::gfx::memory::Buffer& frame_buffer(uint32_t slot) const { return *ubos_[slot % frames_]; }
    uint32_t frames() const { return frames_; }

    /** @brief Bakes the noise textures if they have not been yet. Outside any render pass. */
    void ensure_noise(coopa::gfx::command::CommandBuffer& cmd);
    /** @brief The sampler to bind output_view() with: nearest (the upsample texelFetches it). */
    const coopa::gfx::engine::util::Sampler& sampler() const { return nearest_; }
    uint32_t width() const { return output_.width(); }
    uint32_t height() const { return output_.height(); }

    /** @brief Clears every target to "no cloud" once, so the bound images are laid out. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd);

    /**
     * @brief The size of the target's top-left region a march at `scale` (0..1] of the
     *        half-resolution target fills. sky_physical.glsl's upsample derives the same region
     *        from the scale with the same rounding.
     */
    static uint32_t scaled_extent(uint32_t full, float scale);

    /**
     * @brief Uploads this frame's parameters, then -- with `trace` -- traces, reconstructs and
     *        stores the history, over the top-left `scale` fraction (per axis) of the
     *        half-resolution target (baking the noise first if it has not been yet). Without
     *        `trace` (the layer faded out entirely) only the parameters are uploaded, for the
     *        shadow map. The camera and light sets go at sets 0 and 1 of the trace. Outside any
     *        render pass.
     */
    void execute(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const coopa::gfx::pipeline::DescriptorSet& light_set, const Params& p, float scale = 1.0f,
                 bool trace = true);

private:
    static uint32_t half_(uint32_t v) { return std::max(1u, (v + 1u) / 2u); }

    static coopa::gfx::engine::passes::FullscreenStageDesc describe_trace_(
            const std::string& vert_spv, const std::string& frag_spv,
            const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
            const coopa::gfx::pipeline::DescriptorSetLayout& light_layout, uint32_t frames);
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_resolve_(
            const std::string& vert_spv, const std::string& frag_spv, uint32_t frames);
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_copy_(const std::string& vert_spv,
                                                                          const std::string& frag_spv);

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
    coopa::gfx::engine::targets::OffscreenTarget output_;    ///< the reconstructed layer (packed, RGBA32F; bound to the lighting pass and the composite)
    coopa::gfx::engine::targets::OffscreenTarget history_;   ///< last frame's output_ (packed, RGBA32F; read by the resolve)
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
