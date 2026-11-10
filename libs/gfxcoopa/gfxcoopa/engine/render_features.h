/**
 * @file render_features.h
 * @brief Feature-flag vocabulary and cross-pass invariants shared by every
 *        gfxcoopa consumer's render config.
 */

#ifndef GFXCOOPA_ENGINE_RENDER_FEATURES_H
#define GFXCOOPA_ENGINE_RENDER_FEATURES_H

#include <glm/glm.hpp>

namespace coopa {
namespace gfx {
namespace engine {

/**
 * @struct IndirectParams
 * @brief The indirect-lighting terms a lighting pass ADDS and an SSR
 *        composite SUBTRACTS (see toyengine's gfx/indirect_specular.glsl and
 *        SsrPass::CompositePushConstants).
 *
 * Held in one struct, and fed to both the lighting pass and the SSR pass
 * from a single instance, so the two can never be fed different values by
 * accident -- a mismatch here is invisible in code review and shows up on
 * screen as a faint reflection-shaped residue where the SSR composite's
 * subtraction fails to cancel what the lighting pass added. Pass this by
 * reference into both the lighting pass's push constants and
 * SsrPass::Params rather than copying individual floats out of it, so
 * there is exactly one thing to plumb through and no way to source the two
 * call sites from different values.
 */
struct IndirectParams {
    float ambient_intensity = 1.0f;  ///< Scales the sky/GI indirect diffuse term.
    float sky_intensity     = 1.0f;  ///< Scales the sky-gradient indirect specular base;
                                      ///< MUST be identical between the lighting pass and SsrPass.
    float ssgi_intensity    = 0.0f;  ///< 0 disables the SSR composite's diffuse-bounce term
                                      ///< (a consumer with no SSGI concept of its own, e.g.
                                      ///< blendy, simply never sets this above its default).
    float ssgi_distance     = 0.5f;  ///< World-space offset along N for the bounce sample point.

    /// The three colours gfx/sky.glsl's sky_gradient() mixes between, straight
    /// up / at the horizon / straight down (engine is Z-up). Defaulted to that
    /// file's own SKY_ZENITH/SKY_HORIZON/SKY_GROUND constants, so a consumer
    /// that never sets them matches what the shader would pick on its own.
    /// Kept in this struct, not passed separately, for the same reason as
    /// ambient_intensity/sky_intensity above: the lighting pass and the SSR
    /// composite must see the same colours or SSR's subtraction leaves a
    /// visible residue.
    glm::vec3 sky_zenith  = glm::vec3(0.05f, 0.18f, 0.55f);
    glm::vec3 sky_horizon = glm::vec3(0.25f, 0.35f, 0.45f);
    glm::vec3 sky_ground  = glm::vec3(0.05f, 0.045f, 0.04f);
};

/**
 * @page render_features_policy Runtime vs. startup-only feature flags
 *
 * Every gfxcoopa consumer's render config follows one naming convention so
 * the two tiers stay visually distinguishable at the declaration site:
 *
 *   - Fields ending in `_enabled` are RUNTIME flags: the pass(es) they
 *     gate are always constructed, and the flag is re-read every frame to
 *     decide whether to execute/composite them. Flipping one of these at
 *     runtime (a debug UI, a config hot-reload) takes effect on the next
 *     frame with no pipeline rebuild.
 *   - Fields WITHOUT that suffix are STARTUP-ONLY: they change a pipeline
 *     layout, a descriptor set layout, or a target's format/extent, and are
 *     read once at construction. Changing one requires re-constructing the
 *     owning pipeline (or, for a handful, an explicit `recreate()`/resize
 *     path).
 *
 * | Tier | Typical fields |
 * |---|---|
 * | Runtime | `shadows_enabled`, `ssao_enabled`, `ssr_enabled`, `transparency_enabled`, `skybox_enabled`, `outline_enabled`, `palette_enabled`, `dither_enabled`, `*_temporal_enabled` |
 * | Startup | `gi_enabled` (adds a descriptor set to two pipeline layouts), `ssr_half_res`, `msaa_4x`, every resolution/format field |
 *
 * `IndirectParams::ssgi_intensity` is a value, not a flag: 0.0 disables the
 * diffuse-bounce term entirely inside the shared shader body with no C++
 * branch, which is what makes SSGI a free, always-available option for a
 * consumer whose composite shader implements it and a total no-op for one
 * that doesn't.
 */

} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_RENDER_FEATURES_H
