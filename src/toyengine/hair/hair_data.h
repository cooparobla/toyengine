/**
 * @file hair_data.h
 * @brief Growing a groom and turning it into triangles. Pure data and pure functions -- no scene,
 *        no device, no asset -- so every decision here is unit-testable headless.
 *
 * The same split `toyengine/curve/curve_data.h` makes, and for the same reason: the generator, the
 * simulation and the editor must agree on where a strand is, and they do so by sharing this code
 * rather than by three implementations happening to match.
 *
 * The pipeline is four steps, each a function below:
 *
 *   scatter_roots()  area-weighted points over the scalp's triangles -> StrandSet::roots
 *   grow_strands()   roots -> the points of each strand (droop, curl, clump, frizz)
 *   build_geometry() points -> MeshCpuData (ribbons or tubes, LOD table included)
 *   write_strand_vertices()  rewrites ONE strand's vertices in place, for the simulated path
 *
 * ### Space
 *
 * Everything is in the SCALP OBJECT'S LOCAL space, and the runtime child that draws the groom is
 * given an identity transform -- so the scalp's world matrix applies exactly once. (Procgraph
 * publishes world-space geometry instead, which is why its baked subtree must not inherit a
 * transform; this module deliberately does not repeat that.)
 *
 * ### The vertex channels, and why
 *
 * `coopa::gfx::engine::data::Vertex` is fixed at {position, normal, uv, tangent} and the G-buffer
 * pipeline builds its vertex layout once, so a strand's attributes have to fit those four:
 *
 * | Channel | Carries |
 * |---|---|
 * | `position` | the ribbon or tube corner |
 * | `normal`   | outward from the strand's axis -- radial on a tube, cylindrical on a ribbon |
 * | `uv`       | `(t root-to-tip, per-strand random)` |
 * | `tangent`  | `vec4(strand direction, +1)` |
 *
 * `uv.x` is the one channel every backbone declares (the shadow ones carry position, uv and model
 * and nothing else), which is why the sway hook keys off it.
 *
 * `uv.y` holds the per-strand random rather than a `v` across the strand's width, and `tangent.w`
 * stays a real handedness. Both follow from the backbone: the fragment stage receives only uv and
 * the TBN, `B = cross(N, T) * tangent.w` so anything but +/-1 there corrupts the bitangent, and a
 * `v` across the width would only buy edge alpha -- while a per-strand random is what stops a
 * groom reading as one flat mass. Width tapering is geometry (`thickness_over_length`) and length
 * tapering is the material's alpha mask sampled by `uv.x`.
 *
 * A ribbon's normal leans outward from the strand axis rather than sitting flat on the plane,
 * because crossed cards whose plane faces the light go black -- the problem the tree's leaf cards
 * had, fixed there by blowing their normals outward.
 */

#ifndef TOYENGINE_HAIR_HAIR_DATA_H
#define TOYENGINE_HAIR_HAIR_DATA_H

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/data/mesh.h>

#include "toyengine/curve/curve_data.h"
#include "toyengine/hair/hair_params.h"

namespace toy {
namespace hair {

using Vertex      = coopa::gfx::engine::data::Vertex;
using MeshCpuData = coopa::gfx::engine::data::MeshCpuData;

/**
 * @struct ScalpMesh
 * @brief A non-owning view of the surface hair grows from: a `MeshCpuData`'s triangles.
 */
struct ScalpMesh {
    const Vertex*        vertices     = nullptr;
    std::size_t          vertex_count = 0;
    const std::uint32_t* indices      = nullptr;
    std::size_t          index_count  = 0;

    /// @brief Builds a view over `data`'s LOD 0. The data must outlive the view.
    static ScalpMesh from_cpu_data(const MeshCpuData& data);

    std::size_t triangle_count() const { return index_count / 3; }
    bool        valid() const { return vertices != nullptr && vertex_count > 0 && index_count >= 3; }
};

/**
 * @struct MaskImage
 * @brief An RGBA8 image sampled on the CPU, for `HairParams::root_mask`.
 *
 * On the CPU because the scatter is a CPU pass: a loaded `data::Texture` lives on the GPU with no
 * host copy, so there would be nothing to read.
 */
struct MaskImage {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> rgba;

    bool valid() const;
    /// @brief Bilinear sample, each channel 0..1. UVs wrap. Returns white when invalid, so an
    ///        unset or unreadable mask means "no masking" rather than "no hair".
    glm::vec4 sample(glm::vec2 uv) const;
};

/// @brief Reads `path` as RGBA8 into `out`. @return False when missing or undecodable.
bool load_mask_image(const std::string& path, MaskImage& out);

/**
 * @struct StrandRoot
 * @brief Where one strand starts and what makes it differ from its neighbours.
 */
struct StrandRoot {
    glm::vec3     position{0.0f};   ///< Scalp-local.
    glm::vec3     normal{0.0f};     ///< Interpolated scalp normal, normalised.
    glm::vec3     direction{0.0f};  ///< Where the strand actually leaves: `normal` plus frizz.
    glm::vec2     uv{0.0f};         ///< Scalp UV at the root, for the mask.
    float         random = 0.0f;    ///< Per-strand, 0..1. Rides in every vertex's `uv.y`.
    float         length = 0.0f;    ///< Metres, after jitter and the mask's green channel.
    std::uint32_t clump  = 0;       ///< Index of the clump centre this strand gathers toward.
    /// @brief Which scalp triangle this root grew on, and where in it. Kept so a root can be
    ///        recomputed on a DEFORMING scalp: the same barycentric point on the same triangle,
    ///        read from wherever skinning has since put its three corners.
    std::uint32_t triangle = 0;
    glm::vec3     bary{0.0f};
};

/**
 * @struct StrandSet
 * @brief A whole groom: its roots, and every strand's points in one flat array.
 *
 * Flat rather than a vector per strand because the simulated path rewrites these every frame and a
 * groom is thousands of strands -- the same reason `cloth::Cloth` keeps one flat particle array.
 */
struct StrandSet {
    std::vector<StrandRoot> roots;
    /// @brief `roots.size() * points_per_strand` positions, strand-major. Scalp-local.
    std::vector<glm::vec3>  points;
    int points_per_strand = 0;

    std::size_t strand_count() const { return roots.size(); }
    bool        empty() const { return roots.empty() || points_per_strand < 2; }

    /// @brief The `points_per_strand` positions of strand `i`.
    glm::vec3*       strand(std::size_t i) { return points.data() + i * static_cast<std::size_t>(points_per_strand); }
    const glm::vec3* strand(std::size_t i) const { return points.data() + i * static_cast<std::size_t>(points_per_strand); }
};

/**
 * @struct HairGuide
 * @brief One pen-drawn curve, resampled and expressed in its OWN root frame.
 *
 * A guide shapes strands that do not start where it does, so what is reused is its SHAPE -- the
 * run of offsets from its root, in the frame its own root direction defines -- rather than its
 * positions. Blending positions directly would drag every strand toward the guide's tip.
 */
struct HairGuide {
    glm::vec3 root{0.0f};       ///< Scalp-local.
    glm::vec3 direction{0.0f};  ///< Where it leaves its root; defines the frame `offsets` live in.
    /// @brief `points_per_strand` offsets from the root, in the guide's root frame. [0] is zero.
    std::vector<glm::vec3> offsets;
    float length = 0.0f;        ///< Its arc length, so a strand can scale the shape to its own.

    bool valid() const { return offsets.size() >= 2 && length > 1e-6f; }
};

/**
 * @brief Turns a curve into a guide: resampled evenly by arc length, in its own root frame.
 *
 * @param curve  The guide's path, already in the SCALP'S local space.
 * @param points Points to resample to -- `StrandSet::points_per_strand`.
 * @return A guide; `valid()` is false for a curve too short or degenerate to define a frame.
 */
HairGuide make_hair_guide(const toy::curve::CurveData& curve, int points);

// --- Generation --------------------------------------------------------------------------------

/// @brief Total surface area of the scalp's triangles, in square metres of its LOCAL space.
float scalp_area(const ScalpMesh& scalp);

/**
 * @brief How many strands `params` asks for on `scalp`: `density` x area, capped at `max_strands`.
 *
 * Separated out so the editor can show the count a slider is about to produce before producing it.
 */
std::size_t strand_count_for(const ScalpMesh& scalp, const HairParams& params);

/**
 * @brief Scatters strand roots over the scalp's triangles, weighted by triangle AREA.
 *
 * Area weighting is what makes density mean the same thing on a coarse triangle and a fine one;
 * picking triangles uniformly would crowd hair wherever the mesh happens to be subdivided.
 *
 * Every choice comes from `hash01()` of (seed, strand index, stream) and nothing else, so the same
 * seed grooms identically on every run, thread and machine. `mask`'s red channel thins the groom
 * by rejection -- a strand whose sample falls below its own random is dropped, which thins evenly
 * instead of biasing toward the mask's bright edges.
 *
 * @param scalp  The surface to grow from.
 * @param params Density, length, jitter, frizz, clumping and the seed.
 * @param mask   Optional; null or invalid means no masking.
 * @return A set with `roots` filled and `points` sized but not yet shaped -- call grow_strands().
 */
StrandSet scatter_roots(const ScalpMesh& scalp, const HairParams& params, const MaskImage* mask);

/**
 * @brief Shapes every strand procedurally: straight out along its direction, then droop, curl and
 *        clumping, finally re-spaced to exactly `length`.
 *
 * Droop grows as the square of the distance along the strand, so the root stays planted and the
 * tip moves most -- the same profile the foliage shader's sway uses, and for the same reason.
 *
 * The last step walks the shaped direction field laying down segments of exactly
 * `length / segments`. That makes a strand inextensible however hard droop and curl pull on it,
 * and it gives the solver uniform rest lengths for free.
 *
 * `guides`, when given, is blended in between the procedural shape and the clumping: each strand
 * takes the SHAPE of its nearest guides -- their offsets replanted in its own root frame and
 * scaled to its own length -- so a handful of drawn curves sweep thousands of strands without
 * dragging any of them off the scalp. `HairParams::guide_influence` is how much they win by.
 *
 * @param set        A scattered set; its `points` are overwritten.
 * @param params     Length, droop, curl, clumping, guide influence and count.
 * @param down_local Gravity, in the SCALP'S LOCAL space -- normally the scalp's inverse world
 *                   rotation applied to world down. Passed in rather than assumed, because this
 *                   function may not read a scene and a rotated head still droops downward.
 * @param guides     Guides already in the scalp's local space; empty means purely procedural.
 */
void grow_strands(StrandSet& set, const HairParams& params, const glm::vec3& down_local,
                  const std::vector<HairGuide>& guides = {});

// --- Geometry ----------------------------------------------------------------------------------

/// @brief Vertices one strand contributes, for the shape `params` asks for.
std::size_t vertices_per_strand(const HairParams& params);

/// @brief Indices one strand contributes.
std::size_t indices_per_strand(const HairParams& params);

/**
 * @brief A permutation of `0..count-1` whose every PREFIX is spread evenly over the whole range.
 *
 * Bit-reversal: strand 0, then the middle one, then the quarter points, and so on. Emitting index
 * blocks in this order makes LOD k -- the first `count >> k` blocks -- a thinned-but-even groom
 * rather than a bald patch, with no second generation pass and no extra buffer.
 */
std::vector<std::uint32_t> strand_lod_order(std::size_t count);

/**
 * @brief Builds the whole groom's mesh: vertices in strand order, indices in LOD order.
 *
 * Vertices stay in natural strand order so the simulated path can rewrite strand `i`'s block at
 * `i * vertices_per_strand()` without touching anything else; the index buffer is what carries the
 * bit-reversed order, and `out.lods` is a run of prefixes of it.
 *
 * Levels share one vertex and one index buffer, so the extra levels cost no memory. They do NOT
 * widen the surviving strands to compensate -- width is baked per vertex and every level reads the
 * same vertices -- so a distant groom genuinely thins. That is the trade for free LOD.
 *
 * @param set    A grown strand set.
 * @param params Shape, thickness and the LOD thresholds.
 * @param out    Overwritten with vertices, indices, the LOD table and `cull_screen_size`.
 */
void build_geometry(const StrandSet& set, const HairParams& params, MeshCpuData& out);

/**
 * @brief Rewrites one strand's vertices from its (moved) points, for the simulated path.
 *
 * Matches build_geometry()'s layout exactly: writes `vertices_per_strand()` entries starting at
 * `strand_index * vertices_per_strand()`. Indices are untouched, so a frame's upload is only the
 * vertices that moved.
 *
 * @param set          The set whose `points` have been updated.
 * @param params       Must be the same params build_geometry() was called with.
 * @param strand_index Which strand to rewrite.
 * @param vertices     The whole groom's vertex array, at least `strand_count * per_strand` long.
 */
void write_strand_vertices(const StrandSet& set, const HairParams& params,
                           std::size_t strand_index, std::vector<Vertex>& vertices);

} // namespace hair
} // namespace toy

#endif // TOYENGINE_HAIR_HAIR_DATA_H
