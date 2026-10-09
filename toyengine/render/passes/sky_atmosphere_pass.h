/**
 * @file sky_atmosphere_pass.h
 * @brief The physical sky's look-up tables (Hillaire 2020): transmittance (256 x 64) and
 *        multiple scattering (32 x 32), re-rendered only when the atmosphere changes, and the
 *        sky-view LUT (192 x 108) rendered every frame from the sun's and moon's directions.
 *
 * Fragment passes over small OffscreenTargets (gfxcoopa has no compute). The lighting pass's sky
 * branch and the cloud march (SkyCloudPass) sample the transmittance and sky-view tables -- see
 * assets/shaders/sky_atmosphere.glsl for the parameterisations and sky_physical.glsl for the use.
 *
 * Every target exists from construction and is bound once (PixelRenderPipeline's frame-overlap
 * rule), so render `sky_model` switches live. While the gradient sky is on nothing is recorded:
 * initialize() clears the tables once so the bound images are validly laid out, and that is all.
 */

#ifndef TOYENGINE_RENDER_PASSES_SKY_ATMOSPHERE_PASS_H
#define TOYENGINE_RENDER_PASSES_SKY_ATMOSPHERE_PASS_H

#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/types/texture_view.h>

#include <toyengine/render/sky_state.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class SkyAtmospherePass
 * @brief See the file doc.
 */
class SkyAtmospherePass {
public:
    static constexpr uint32_t kTransmittanceW = 256, kTransmittanceH = 64;
    static constexpr uint32_t kMultiscatterSize = 32;
    static constexpr uint32_t kViewW = 192, kViewH = 108;

    /// sky_atmosphere.glsl's SkyAtmosphere, byte for byte.
    struct AtmosphereGpu {
        glm::vec4 rayleigh{0.0f};   ///< rgb scattering (1/km), w scale height (km)
        glm::vec4 mie{0.0f};        ///< rgb scattering (1/km), w scale height (km)
        glm::vec4 mie_ext{0.0f};    ///< rgb extinction (1/km), w phase g
        glm::vec4 ozone{0.0f};      ///< rgb absorption (1/km), w bottom radius (km)
        glm::vec4 ground{0.0f};     ///< rgb albedo, w top radius (km)

        static AtmosphereGpu of(const SkyAtmosphereMedia& m) {
            AtmosphereGpu g;
            g.rayleigh = glm::vec4(m.rayleigh_scattering, m.rayleigh_scale_height);
            g.mie = glm::vec4(m.mie_scattering, m.mie_scale_height);
            g.mie_ext = glm::vec4(m.mie_extinction, m.mie_g);
            g.ozone = glm::vec4(m.ozone_absorption, m.bottom_radius);
            g.ground = glm::vec4(m.ground_albedo, m.top_radius);
            return g;
        }
    };
    static_assert(sizeof(AtmosphereGpu) == 80, "AtmosphereGpu must match sky_atmosphere.glsl's SkyAtmosphere");

    /// sky_view.frag's push constants.
    struct ViewParams {
        AtmosphereGpu atmo;
        glm::vec4 sun{0.0f, 0.0f, 1.0f, 0.0f};   ///< xyz to the sun, w moon / sun illuminance
        glm::vec4 view{6360.01f, 30.0f, 0.0f, 0.0f};  ///< x camera radius (km), y steps
    };
    static_assert(sizeof(ViewParams) == 112, "ViewParams must match sky_view.frag's push constants");

    SkyAtmospherePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
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

    SkyAtmospherePass(const SkyAtmospherePass&) = delete;
    SkyAtmospherePass& operator=(const SkyAtmospherePass&) = delete;

    coopa::gfx::TextureView transmittance_view() const { return transmittance_.color_view_typed(); }
    coopa::gfx::TextureView sky_view_view() const { return view_.color_view_typed(); }
    const coopa::gfx::engine::util::Sampler& sampler() const { return linear_; }

    /** @brief Clears every table once, so the bound images are laid out before anything samples them. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd) {
        if (initialized_) return;
        const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 1.0f}};
        transmittance_.begin(cmd, zero); transmittance_.end(cmd);
        multiscatter_.begin(cmd, zero); multiscatter_.end(cmd);
        view_.begin(cmd, zero); view_.end(cmd);
        initialized_ = true;
    }

    /** @brief Re-renders the transmittance and multi-scattering tables when `media` changed. */
    void update_luts(coopa::gfx::command::CommandBuffer& cmd, const SkyAtmosphereMedia& media) {
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

    /** @brief Renders the sky-view table for this frame's sun and camera height. */
    void render_view(coopa::gfx::command::CommandBuffer& cmd, const ViewParams& params) {
        view_.begin(cmd);
        view_stage_.draw(cmd, kViewW, kViewH, coopa::gfx::ShaderStage::Fragment, params);
        view_.end(cmd);
    }

    /** @brief How many times the transmittance / multi-scatter tables were rendered (tests). */
    uint32_t lut_renders() const { return lut_renders_; }

private:
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_(const std::string& vert_spv, const std::string& frag_spv,
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

    coopa::gfx::engine::util::Sampler            linear_;
    coopa::gfx::engine::targets::OffscreenTarget transmittance_;
    coopa::gfx::engine::targets::OffscreenTarget multiscatter_;
    coopa::gfx::engine::targets::OffscreenTarget view_;
    coopa::gfx::engine::passes::FullscreenStage  transmittance_stage_;
    coopa::gfx::engine::passes::FullscreenStage  multiscatter_stage_;
    coopa::gfx::engine::passes::FullscreenStage  view_stage_;
    SkyAtmosphereMedia lut_media_;
    bool luts_valid_ = false;
    bool initialized_ = false;
    uint32_t lut_renders_ = 0;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_SKY_ATMOSPHERE_PASS_H
