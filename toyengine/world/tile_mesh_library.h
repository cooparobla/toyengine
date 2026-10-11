/**
 * @file tile_mesh_library.h
 * @brief A terrain's tile sets, baked: every piece of every set in its eight orientations, the
 *        section caps derived from them, and the primitive that stamps one into a chunk's merged
 *        vertex/index buffers.
 *
 * A tile set (assets/objects/terrain/tileset_*.yaml) is authored once as ordinary mesh YAMLs,
 * one per TilePiece, in the canonical frames tile_topology.h describes. This class bakes each
 * piece into its eight D4 orientations ONCE at load, and from then on stamping a piece into a
 * chunk is a scale, a translate and an index rebase. Nothing downstream knows whether a set is
 * rounded, chamfered or square -- which is what lets a TileTerrain change form by pointing at
 * another tile set.
 *
 * ### Why the geometry is copied rather than referenced
 *
 * Chunk meshing runs on coopa::job worker threads while the main thread may still be
 * publishing assets. AssetHandle is a refcounted view onto a slot the AssetManager mutates, so
 * a worker dereferencing one races that. bake_style_piece() therefore takes a plain `const
 * SkinnedMeshSource&` on the main thread and **copies** the vertices out; a baked library is
 * immutable, self-contained, and safe for unlimited concurrent readers. It also means a test
 * can build a library from hand-written triangles without an AssetManager, a Device, or a file.
 *
 * ### Why SkinnedMeshSource
 *
 * coopa::gfx::engine::data::SkinnedMeshSource is gfxcoopa's CPU-only mesh payload (see its file
 * doc): its `joints`/`joint_weights` keys are optional, so it parses a plain mesh YAML verbatim
 * -- quads triangulated, tangents generated when absent -- and holds no GPU resources. Its
 * loader is already registered by toy::core::Engine, so authored pieces need no new asset
 * type and no new loader. A plain coopa::gfx::engine::data::Mesh would be the wrong tool: it
 * uploads to the GPU and does not retain the CPU arrays a merge needs.
 */

#ifndef TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
#define TOYENGINE_WORLD_TILE_MESH_LIBRARY_H

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/world/tile_topology.h>

namespace toy {
namespace world {

/**
 * @class TileMeshLibrary
 * @brief A terrain's baked tile sets ("styles"), and append_styled() to stamp a piece.
 *
 * @code
 * TileMeshLibrary lib;
 * const std::uint8_t round = lib.add_style();
 * for (each piece) lib.bake_style_piece(round, piece, *source);
 * lib.finish_styles();                       // fallbacks for missing pieces; now is_baked()
 * lib.set_kind_style(TileKind::Stone, rock);  // optional per-kind sets
 * @endcode
 */
class TileMeshLibrary {
public:
    using Vertex = coopa::gfx::engine::data::Vertex;
    using SkinnedMeshSource = coopa::gfx::engine::data::SkinnedMeshSource;

    /**
     * @struct SideGeometry
     * @brief One baked piece (or section cap, or foot) at one orientation, in the unit cell
     *        `[0,1]^3`; append_styled() applies the cell's scale and origin.
     */
    struct SideGeometry {
        std::vector<Vertex>        vertices;
        std::vector<std::uint32_t> indices;

        bool empty() const { return vertices.empty() || indices.empty(); }
    };

    /** @brief True once finish_styles() has run over at least one style -- chunks can build. */
    bool is_baked() const { return finished_; }

    // ------------------------------------------------------------------
    // Styled tiles -- see tile_topology.h for the piece vocabulary and frames.
    // ------------------------------------------------------------------

    /// Which plane of a wall piece a section cap lies on.
    enum class SectionPlane : std::uint8_t {
        Bottom, /**< z = 0, facing down. */
        Top,    /**< z = 1, facing up (plain tier only). */
        End     /**< The end the piece reaches: x = 1 (Continue) or the corner bisector (Concave). */
    };
    static constexpr std::size_t k_section_plane_count = 3;

    using Oriented = std::array<SideGeometry, k_tile_orientation_count>;

    /**
     * @struct StyleSet
     * @brief One tile style: every piece baked in all eight orientations, plus the section caps
     *        derived from them.
     */
    struct StyleSet {
        std::array<Oriented, k_tile_piece_count> pieces;
        std::array<std::array<Oriented, k_section_plane_count>, k_tile_piece_count> sections;
        std::array<bool, k_tile_piece_count> authored{};

        /// straight[cap][turns]: the straight wall half stretched over a whole unit of its
        /// along-wall axis (x in [0,1] instead of [0.5,1]), turned onto each face -- so a run of
        /// straight halves can be stamped as ONE strip (see append_styled()'s `stretch`).
        std::array<std::array<SideGeometry, 4>, 2> straight;

        /// The tile set's `round_kind_corners`: whether a top of this style blends into its
        /// neighbours' kinds at a convex corner (terrain_styled.frag rounds it). False keeps
        /// kind boundaries square -- what a blocky set wants.
        bool round_kind_corners = true;
    };

    /** @brief Adds an empty style and returns its index. Kinds map to style 0 until told otherwise. */
    std::uint8_t add_style();

    /** @brief Number of styles (tile sets). */
    std::size_t style_count() const { return styles_.size(); }

    /** @brief True once any style exists. */
    bool has_styles() const { return !styles_.empty(); }

    /** @brief Sets a style's `round_kind_corners` (see StyleSet). */
    void set_round_kind_corners(std::uint8_t style, bool round) { styles_[style].round_kind_corners = round; }

    /** @brief Whether tops of `style` round the corners where surface kinds meet. */
    bool rounds_kind_corners(std::uint8_t style) const {
        return style < styles_.size() && styles_[style].round_kind_corners;
    }

    /** @brief Assigns the style a surface kind's cells are shaped with. */
    void set_kind_style(TileKind kind, std::uint8_t style) {
        kind_style_[static_cast<std::size_t>(kind)] = style;
    }

    /** @brief The style a surface kind's cells are shaped with. */
    std::uint8_t style_of(TileKind kind) const;

    /**
     * @brief Bakes one authored piece of a style into all eight orientations, and derives its
     *        section caps.
     *
     * Call finish_styles() once every piece that will arrive has been baked.
     *
     * @param style  Index from add_style().
     * @param piece  Which piece this mesh is.
     * @param source Authored in the piece's canonical frame (see tile_topology.h).
     */
    void bake_style_piece(std::uint8_t style, TilePiece piece, const SkinnedMeshSource& source);

    /**
     * @brief Fills every piece a style did not author from its nearest relative, so a partial
     *        style degrades to simpler shapes rather than punching holes in the world.
     *
     * Cap tiers fall back to their plain tier, plain corners to the plain straight wall, the
     * corner quadrants to the edge quadrant and that to the inner one.
     */
    void finish_styles();

    /** @brief True when `style` can draw `piece` (authored, or filled in by finish_styles()). */
    bool has_piece(std::uint8_t style, TilePiece piece) const {
        return style < styles_.size() && !styles_[style].pieces[static_cast<std::size_t>(piece)][0].empty();
    }

    /** @brief One baked piece, at one orientation. */
    const SideGeometry& piece(std::uint8_t style, TilePiece piece, std::uint8_t orientation) const {
        return styles_[style].pieces[static_cast<std::size_t>(piece)][orientation & 7u];
    }

    /** @brief A wall piece's derived section cap on one of its planes. Empty when it has none. */
    const SideGeometry& section(std::uint8_t style, TilePiece piece, SectionPlane plane,
                                std::uint8_t orientation) const;

    /**
     * @brief A straight wall half spanning its whole along-wall unit, for stamping merged runs.
     * @param cap   The cap tier (with the lip) or the plain tier.
     * @param turns lateral_quarter_turns() of the face.
     */
    const SideGeometry& straight(std::uint8_t style, bool cap, std::uint8_t turns) const {
        return styles_[style].straight[cap ? 1 : 0][turns & 3u];
    }

    /** @brief A whole flat top at z = 1 -- the fast path for a tile with no exposed edge. */
    const SideGeometry& full_top() const { return full_top_; }

    /**
     * @brief The floor triangle under a convex wall half, at z = 0: the corner of its own
     *        footprint the rounding carved away, which no neighbouring top covers.
     */
    const SideGeometry& foot(std::uint8_t orientation) const { return foot_[orientation & 7u]; }



    /**
     * @brief Stamps one styled piece (or section cap, or foot) into a chunk's buffers.
     *
     * Every vertex carries `blend_code` in uv.x (encode_surface_blend()): terrain_styled.frag
     * reads which kinds the surface may show, and decides per pixel. A `top_anchored`
     * piece -- the cap tier -- keeps everything above its z = 0.5 at tile proportions, pinned
     * to the cell top, and stretches only the band below to fill the step: a lip authored for a
     * cubic cell therefore stays round on a tall step instead of turning into an ellipse.
     * Steps shorter than half a tile fall back to plain linear scaling.
     *
     * @param geometry     What to stamp.
     * @param origin       World position of the cell's minimum corner.
     * @param scale        `{tile_size, tile_size, height_step}`.
     * @param blend_code   The surface's packed kinds (encode_surface_blend()).
     * @param top_anchored Use the cap tier's anchored vertical mapping.
     * @param out_v        Vertex buffer to append to.
     * @param out_i        Index buffer to append to; rebased onto `out_v`'s current end.
     * @param stretch_axis World axis (0 = x, 1 = y) `stretch` lengthens.
     * @param stretch      Length multiplier along `stretch_axis`, for a straight() run; only
     *                     valid for geometry invariant along that axis.
     */
    void append_styled(const SideGeometry& geometry, const glm::vec3& origin, const glm::vec3& scale,
                       float blend_code, bool top_anchored,
                       std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                       int stretch_axis = 0, float stretch = 1.0f) const;

private:
    /// A mesh source flattened to a plain triangle list (three vertices per triangle).
    static std::vector<Vertex> triangle_soup_(const SkinnedMeshSource& source);

    /**
     * Orients a canonical triangle soup eight ways. Mirrored orientations reverse the winding.
     * UVs and tangents are not baked: append_styled() writes the surface's blend code and a
     * normal-derived tangent, because terrain_styled.frag needs nothing else.
     */
    static void bake_oriented_(const std::vector<Vertex>& soup, Oriented& out);

    /**
     * Derives a piece's solid cross-section on one of its boundary planes -- a **section cap**.
     *
     * The piece's boundary edges lying on the plane trace its profile there; chained in order
     * (starting nearest `anchor_start`, gaps bridged straight), closed with the optional anchors,
     * and fanned from `ref`, they cover the piece's solid side of the plane. Emitted against a
     * neighbouring piece of a different style or shape, the part of this cap inside the
     * neighbour's solid is hidden by the neighbour's own surface, and the part that is not is
     * exactly the step between the two profiles. So caps are per piece, never per pair -- and
     * because they are derived, a hand-authored piece gets them too.
     *
     * Requires the section to be star-shaped about `ref`, which every piece the generator
     * writes satisfies (see tools/gen_tile_styles.py).
     *
     * @return A canonical triangle soup, wound to face `normal`.
     */
    static std::vector<Vertex> derive_section_(const std::vector<Vertex>& soup, const glm::vec3& point,
                                               const glm::vec3& normal, const glm::vec3& ref,
                                               const glm::vec3* anchor_start, const glm::vec3* anchor_end);

    /**
     * The piece's boundary edges lying on a plane, as one ordered polyline: chained, then
     * concatenated greedily by nearest endpoint starting from `start` (gaps bridged straight).
     */
    static std::vector<glm::vec3> boundary_chain_(const std::vector<Vertex>& soup, const glm::vec3& point,
                                                  const glm::vec3& normal, const glm::vec3& start);

    /// Fans a polygon from `ref` into a triangle soup facing `normal`, skipping slivers.
    static std::vector<Vertex> fan_(const std::vector<glm::vec3>& polygon, const glm::vec3& ref,
                                    const glm::vec3& normal);

    /// The style-independent pieces: the whole flat top and the convex foot.
    void bake_fixed_pieces_();

    std::vector<StyleSet>                     styles_;
    std::array<std::uint8_t, k_tile_kind_count> kind_style_{};
    SideGeometry                              full_top_;
    Oriented                                  foot_;

    bool finished_ = false; /**< finish_styles() ran; reset by add_style(). */
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
