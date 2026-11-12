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
 * Every target exists from construction and is bound once (ToyRenderPipeline's frame-overlap
 * rule), so render `sky_model` switches live. While the gradient sky is on nothing is recorded:
 * initialize() clears the tables once so the bound images are validly laid out, and that is all.
 */

#ifndef TOYENGINE_RENDER_PASSES_SKY_ATMOSPHERE_PASS_H
#define TOYENGINE_RENDER_PASSES_SKY_ATMOSPHERE_PASS_H

#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>

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

        static AtmosphereGpu of(const SkyAtmosphereMedia& m);
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
                      const std::string& multiscatter_spv, const std::string& view_spv);

    SkyAtmospherePass(const SkyAtmospherePass&) = delete;
    SkyAtmospherePass& operator=(const SkyAtmospherePass&) = delete;

    coopa::gfx::TextureView transmittance_view() const { return transmittance_.color_view_typed(); }
    coopa::gfx::TextureView sky_view_view() const { return view_.color_view_typed(); }
    const coopa::gfx::engine::util::Sampler& sampler() const { return linear_; }

    /** @brief Clears every table once, so the bound images are laid out before anything samples them. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd);

    /** @brief Re-renders the transmittance and multi-scattering tables when `media` changed. */
    void update_luts(coopa::gfx::command::CommandBuffer& cmd, const SkyAtmosphereMedia& media);

    /** @brief Renders the sky-view table for this frame's sun and camera height. */
    void render_view(coopa::gfx::command::CommandBuffer& cmd, const ViewParams& params);

    /** @brief How many times the transmittance / multi-scatter tables were rendered (tests). */
    uint32_t lut_renders() const { return lut_renders_; }

private:
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_(const std::string& vert_spv, const std::string& frag_spv,
                                                                     int samplers, size_t push_bytes);

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
