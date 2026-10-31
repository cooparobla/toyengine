/**
 * @file hair_params.h
 * @brief `HairParams` -- everything a groom is authored from, and nothing derived.
 *
 * Pure data: no scene, no device, no asset. The `Hair` component holds one, the generator in
 * hair_data.h reads one, and the editor's inspector edits one through the component's YAML. Every
 * length is in METRES and every angle in degrees, matching the rest of the engine.
 *
 * `hash()` is what makes `regenerate: on_change` affordable -- the system re-scatters only when
 * the hash moves, rather than on every inspector keystroke (the editor destroys and recreates a
 * component on each one).
 */

#ifndef TOYENGINE_HAIR_HAIR_PARAMS_H
#define TOYENGINE_HAIR_HAIR_PARAMS_H

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "toyengine/particles/particle_math.h"

namespace toy {
namespace hair {

/// @brief What one strand is built out of.
enum class HairShape : std::uint8_t {
    /// Flat strips, `cross_section` of them crossed about the strand axis. Alpha-tested, and what
    /// dense fine hair and fur want: two triangles per step per plane.
    Ribbon,
    /// An extruded `sides`-gon. Fully opaque -- no alpha test, no sorting -- and what a few
    /// deliberately thick stylised strands want.
    Tube,
};

/// @brief When the groom is rebuilt.
enum class HairRegenerate : std::uint8_t {
    OnLoad,    ///< Once, during warm-up. A groom that never changes costs nothing after that.
    OnChange,  ///< Whenever `HairParams::hash()` or a guide moves. The editing default.
    Manual,    ///< Only on an explicit `regenerate()`. For pinning a heavy groom while laying out.
};

/// @brief The YAML spelling of a shape, for the editor's save path.
const char* hair_shape_name(HairShape shape);

/// @brief Parses a shape name; returns `Ribbon` for anything unrecognised.
HairShape parse_hair_shape(const std::string& name);

/// @brief The YAML spelling of a regeneration mode.
const char* hair_regenerate_name(HairRegenerate mode);

/// @brief Parses a regeneration mode; returns `OnChange` for anything unrecognised.
HairRegenerate parse_hair_regenerate(const std::string& name);

/**
 * @struct HairParams
 * @brief The authored groom. Defaults describe short, fine human hair.
 */
struct HairParams {
    // --- Shape ---------------------------------------------------------------------------------

    HairShape shape = HairShape::Ribbon;
    /// @brief Ribbon only: crossed planes per strand, 1..3. More than one stops a strand
    ///        disappearing when the view is edge-on to its plane, the trick foliage cards use.
    int  cross_section = 2;
    /// @brief Tube only: sides of the extruded cross section, 3..16.
    int  sides = 5;
    /// @brief Points along one strand, so `segments + 1` positions. Also the particle count per
    ///        simulated guide.
    int  segments = 8;

    // --- Growth --------------------------------------------------------------------------------

    /// @brief Strands per SQUARE METRE of scalp, so a bigger head grows proportionally more.
    float density = 20000.0f;
    /// @brief Ceiling on the strand count whatever `density` and the scalp's area imply. A groom
    ///        that would exceed it is thinned, not truncated -- the distribution stays even.
    int   max_strands = 8000;
    float length = 0.12f;
    /// @brief Fraction of `length` randomised away per strand, 0..1.
    float length_jitter = 0.25f;
    /// @brief Metres across at the root. 0.001 is a human hair; 0.03 is a thick poly strand.
    float thickness = 0.0012f;
    /// @brief Multiplies `thickness` along the strand, root (0) to tip (1). Empty = no taper.
    particles::FloatCurve thickness_over_length = particles::FloatCurve::linear(1.0f, 0.25f);

    // --- Styling -------------------------------------------------------------------------------

    /// @brief How far a strand bends downward over its length, in multiples of its own length.
    ///        0 stands straight off the scalp; 1 is a strand whose tip has fallen its full length.
    float droop = 0.4f;
    /// @brief How strongly strands gather into locks, 0..1. Pulls each strand toward its clump
    ///        centre, more so toward the tip, which is how real hair separates.
    float clump = 0.3f;
    /// @brief Number of clump centres as a fraction of the strand count, 0..1.
    float clump_density = 0.02f;
    /// @brief Turns of a helix over the strand's length. 0 is straight.
    float curl = 0.0f;
    /// @brief Radius of that helix, in metres.
    float curl_radius = 0.01f;
    /// @brief Randomises each strand's direction away from the scalp normal, in degrees.
    float frizz_degrees = 6.0f;

    // --- Masking and guides --------------------------------------------------------------------

    /// @brief Optional image sampled by the scalp's UV: R scales density, G scales length.
    ///        Read on the CPU (the scatter is a CPU pass), so it is a path, not a Texture handle.
    std::string root_mask;
    /// @brief Name of an object whose descendants' `Curve` components are the guide strands.
    ///        Empty: the groom is purely procedural.
    std::string guides;
    /// @brief How much the guides win over the procedural shape, 0..1.
    float guide_influence = 1.0f;
    /// @brief Guides blended per strand. More is smoother and costs more at generation only.
    int   guide_count = 3;

    // --- Wind ----------------------------------------------------------------------------------

    /// @brief Tip sway in a 10 m/s wind, as a fraction of the strand's own length. Reaches the
    ///        `hair` surface shader through the material's `shader_params`, so it costs nothing
    ///        per frame -- this is what animates a groom that is not simulated.
    float wind_flexibility = 0.25f;
    /// @brief Rad/s of the flutter riding on the steady lean. Speeds up with the wind.
    float wind_frequency = 3.0f;

    // --- Simulation ----------------------------------------------------------------------------

    /// @brief Off: the groom is a static mesh that sways in the vertex shader, costing nothing per
    ///        frame. On: guide strands are solved by physxcoopa and the render strands follow.
    bool  simulate = false;
    /// @brief Guides actually simulated. Render strands are skinned to the nearest of these, so
    ///        this -- not `density` -- is what the solver costs scale with.
    int   sim_guides = 64;

    /// @brief How hard a strand is pulled back to the shape it was combed into, as the fraction
    ///        of the way back per 1/60 s. The main stiffness control: 0 is limp hair that forgets
    ///        its groom, 1 is a wig that barely reacts. Pushed into the live solver each frame,
    ///        so tuning it in the inspector takes effect without re-scattering the groom.
    float sim_stiffness = 0.12f;
    /// @brief How fast that fades from root to tip. Higher frees the tips sooner, which is what
    ///        makes hair swing from its ends rather than as one board.
    float sim_stiffness_falloff = 1.6f;
    /// @brief How hard each segment's rest ANGLE is restored, per 1/60 s. What holds a curl.
    float sim_curl_hold = 0.25f;
    /// @brief Velocity damping. Higher settles sooner.
    float sim_damping = 0.06f;
    /// @brief Drag against the air, which is what makes hair trail behind a moving head.
    float sim_drag = 0.6f;
    /// @brief Metres a strand is held off a collider. Hair has no volume of its own, so without
    ///        an offset it would sit exactly on the skin and z-fight it.
    float sim_collision_thickness = 0.01f;
    /// @brief Coulomb friction at contacts: 1 grips, so hair resting on a shoulder stays there.
    float sim_friction = 0.4f;
    /// @brief Multiplies world gravity on the strands.
    float sim_gravity = 1.0f;

    // --- Level of detail -----------------------------------------------------------------------

    /// @brief Projected height, as a fraction of the screen's, below which each successive level
    ///        is drawn. Each halves the strand count. Empty disables hair LOD.
    glm::vec2 lod_screen_sizes{0.09f, 0.035f};
    /// @brief Below this projected height nothing draws the groom at all. 0 never culls.
    float cull_screen_size = 0.012f;

    // --- Bookkeeping ---------------------------------------------------------------------------

    /// @brief Seeds every random choice, so a groom is repeatable. Change it for a different head.
    std::uint32_t  seed = 7;
    HairRegenerate regenerate = HairRegenerate::OnChange;

    /// @brief Clamps every field to the range its doc claims. Called after parsing and before use,
    ///        so a hand-edited YAML cannot ask for 0 sides or a negative length.
    void sanitize();

    /**
     * @brief A content hash of every field that changes the generated geometry.
     *
     * `regenerate: on_change` compares this between frames. Fields that do NOT affect geometry
     * (the LOD thresholds, `cull_screen_size`, the wind response, and every `sim_*` tunable but
     * `sim_guides`) are deliberately excluded, so tuning them does not trigger a re-scatter --
     * they are pushed to the material or straight into the live solver instead.
     */
    std::uint64_t hash() const;
};

} // namespace hair
} // namespace toy

#endif // TOYENGINE_HAIR_HAIR_PARAMS_H
