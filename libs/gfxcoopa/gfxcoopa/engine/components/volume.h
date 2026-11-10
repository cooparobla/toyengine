/**
 * @file volume.h
 * @brief Scene component defining a local, bounded, raymarched volume.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_VOLUME_H
#define GFXCOOPA_ENGINE_COMPONENTS_VOLUME_H

#include <coopa/scene/component.h>
#include <glm/glm.hpp>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/// Density function a VolumeComponent is evaluated with. Matches
/// VolumeGPU::mode_params.x and gfx/volumetrics.glsl's GFX_VOLUME_KIND_* defines.
enum class VolumeKind {
    Fog,   ///< Uniform density inside the bounds -- a static soft pocket.
    Wind,  ///< Ridged noise -> thin advected ribbons.
    Haze,  ///< Smooth fbm -> drifting billows; a continuous medium whose density varies.
};

/// Shape a VolumeComponent is bounded by. Matches VolumeGPU::extent_shape.w
/// (0 = Box, 1 = Sphere, 2 = Shell).
enum class VolumeShape {
    Box,
    Sphere,
    /// A sphere that is dense near an INNER radius and fades OUTWARD toward `extent.x`, the
    /// opposite sense from Sphere (which peaks at its own centre and fades toward its surface).
    /// `height_base`/`height_falloff` are reused as the inner radius and the altitude decay
    /// scale rather than a world-Z height -- the one shape meant for something that sits ON a
    /// sphere's surface (a planet's atmosphere) rather than floating free in world space.
    Shell,
};

/**
 * @class VolumeComponent
 * @brief Local, artist-placed, raymarched volume. Always bounded.
 *
 * The single local-volume concept in the engine. Fog is GLOBAL only and lives in
 * the renderer config (LightUBO's fog block, light_data.h); everything local -- including a static
 * fog pocket -- is one of these, raymarched by VolumetricsPass.
 *
 * `kind` selects the density function; every other field means the same thing for
 * all three kinds, which is what lets one march serve all of them.
 *
 * The defaults below are the Wind ones. The three kinds want very different
 * values and a C++ struct can only carry one set, so the SCENE PARSER applies
 * kind-specific defaults after reading `kind` and before reading anything else
 * (see register.h). That is what keeps authoring short: a volume declaring only
 * `kind: haze`, a Transform and an extent comes out looking like haze.
 *
 * IMPORTANT -- this does NOT enable the pass. `volumetrics_enabled` in the
 * renderer config is the master switch and is startup-fixed.
 */
class VolumeComponent : public coopa::scene::Component {
public:
    VolumeComponent() = default;
    std::string type_name() const override { return "VolumeComponent"; }
    void update(float /*dt*/) override {}

    VolumeKind  kind    = VolumeKind::Wind;
    VolumeShape shape   = VolumeShape::Box;
    glm::vec3   extent  = glm::vec3(4.0f);   ///< Local half-extent (Sphere uses .x as radius).
    /// Edge softness, as a fraction of a dimensionless [0,1] surface metric (0 = hard
    /// edge, 1 = maximally soft) -- NOT a world distance or a fraction of `extent`.
    /// Evaluated per raymarch sample (see gfx_fog_box_edge_weight /
    /// gfx_fog_sphere_point_weight), so it looks the same from any viewing angle.
    float       falloff = 0.35f;

    // --- Field ---
    glm::vec3 direction      = glm::vec3(1.0f, 0.3f, 0.0f); ///< World-space advection direction (Z-up); normalized on upload.
    float     speed          = 2.0f;   ///< World units/sec the field is advected. 0 freezes it in place.
    /// How much the scene's weather wind (if the host engine has one) adds to the drift: the
    /// field moves at direction * speed + wind * wind_influence. 0 = only the authored drift.
    float     wind_influence = 0.0f;
    float     density        = 0.30f;  ///< Peak extinction per world unit.
    float     noise_scale    = 0.45f;  ///< Feature frequency; for Wind this sets ribbon THICKNESS as well as spacing.
    float     streak         = 8.0f;   ///< Elongation along `direction`; 1 = isotropic.
    float     coverage       = 0.86f;  ///< Fraction of the volume with no density. Wind gates on a second
                                       ///< noise; Haze thresholds the field itself, so Haze wants ~0.
    float     sharpness      = 3.0f;   ///< Ridge crest exponent (Wind only): 1 = soft ridge, 8 = thin strand.
    float     gate_scale     = 2.5f;   ///< Sparsity-gate frequency. <= 0 disables the gate -- which Haze requires.
    float     flow_warp      = 2.5f;   ///< Meander amplitude in WORLD UNITS; 0 disables the domain warp.
    float     flow_scale     = 0.05f;  ///< Meander frequency; keep well below noise_scale or features dissolve.
    int       octaves        = 2;      ///< fbm octaves, clamped to [1,4] on upload.
    float     detail_gain    = 0.5f;   ///< fbm amplitude falloff per octave.
    float     height_base    = 0.0f;   ///< World Z at/below which the volume is at full density.
    float     height_falloff = 8.0f;   ///< Exponential decay above the base, in world units; <= 0 disables.

    // --- Shading ---
    glm::vec3 color      = glm::vec3(0.75f, 0.78f, 0.85f); ///< Scatter colour.
    float     occlusion  = 0.15f; ///< How much the volume veils the scene. 0 = pure additive glow, 1 = full medium.
    float     sun_amount = 0.6f;  ///< Henyey-Greenstein sun in-scatter strength; 0 = flat colour.
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_VOLUME_H
