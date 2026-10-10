/**
 * @file tile_mesh_library.h
 * @brief The per-side tile geometry, baked into its six orientations, and the primitive that
 *        stamps one side into a chunk's merged vertex/index buffers.
 *
 * This is the file that makes "a tile side is a mesh" true rather than a figure of speech. A
 * side is authored once as an ordinary mesh YAML -- the same Blender-exported schema every
 * other mesh in this repo uses -- in the canonical orientation tile_types.h describes (the +Z
 * face of a unit cube spanning `[0,1]^3`). This class rotates it onto all six faces ONCE at
 * load, and from then on stamping a side into a chunk is a translate, a UV remap and an index
 * rebase. Nothing downstream knows whether that side is a flat quad, a chamfered rim or a
 * curved patch -- which is exactly the seam smoother tiles will arrive through.
 *
 * ### Why the geometry is copied rather than referenced
 *
 * Chunk meshing runs on coopa::job worker threads while the main thread may still be
 * publishing assets. AssetHandle is a refcounted view onto a slot the AssetManager mutates, so
 * a worker dereferencing one races that. bake() therefore takes a plain `const
 * SkinnedMeshSource&` on the main thread and **copies** the vertices out; a baked library is
 * immutable, self-contained, and safe for unlimited concurrent readers. It also means a test
 * can build a library from hand-written triangles without an AssetManager, a Device, or a file.
 *
 * ### Why SkinnedMeshSource
 *
 * coopa::gfx::engine::data::SkinnedMeshSource is gfxcoopa's CPU-only mesh payload (see its file
 * doc): its `joints`/`joint_weights` keys are optional, so it parses a plain mesh YAML verbatim
 * -- quads triangulated, tangents generated when absent -- and holds no GPU resources. Its
 * loader is already registered by toy::core::Engine, so authored side meshes need no new asset
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
 * @brief Six baked side geometries, and append() to stamp one into a chunk buffer.
 *
 * @code
 * TileMeshLibrary lib;
 * lib.bake_canonical(*flat_quad_source);            // all six faces from one authored mesh
 * lib.bake_side(TileFace::Top, *bevelled_source);   // ...overriding just the top
 *
 * lib.append(TileFace::Top, origin, scale, TileKind::Grass, vertices, indices);
 * @endcode
 */
class TileMeshLibrary {
public:
    using Vertex = coopa::gfx::engine::data::Vertex;
    using SkinnedMeshSource = coopa::gfx::engine::data::SkinnedMeshSource;

    /**
     * @struct SideGeometry
     * @brief One face's geometry, already rotated out of canonical space onto that face.
     *
     * Positions still span the unit cube `[0,1]^3`; append() applies the tile's scale and
     * origin. UVs are the authored `[0,1]` ones; append() maps them into an atlas cell.
     */
    struct SideGeometry {
        std::vector<Vertex>        vertices;
        std::vector<std::uint32_t> indices;

        bool empty() const { return vertices.empty() || indices.empty(); }

        /// True when this side is a flat, axis-aligned unit quad whose UVs are an affine
        /// function of position -- the only shape the greedy mesher may stretch over several
        /// tiles (see mesh_chunk_columns_greedy()). Then uv = uv_origin + uv_du * p[axis_u] +
        /// uv_dv * p[axis_v] for any point p on it, extended past [0,1] by append_span().
        bool      mergeable = false;
        int       axis_u    = 0;
        int       axis_v    = 1;
        glm::vec2 uv_origin{0.0f};
        glm::vec2 uv_du{0.0f};
        glm::vec2 uv_dv{0.0f};
    };

    /// Encodes a tile-space UV for the `terrain` surface shader: the atlas cell index rides
    /// in u's high part (cell * k_uv_cell_stride, re-centred by half the stride so a local u
    /// may run negative), the position within the tile repeat in the rest. terrain.frag
    /// decodes it and samples cell + fract(local), so one quad may span many tiles.
    static constexpr float k_uv_cell_stride = 1024.0f;
    static glm::vec2 encode_uv(TileKind kind, const glm::vec2& local);

    /**
     * @brief Bakes one authored mesh onto all six faces.
     *
     * The ordinary case: one `tile_side_flat.yaml` becomes a whole cube's worth of sides.
     * Call bake_side() afterwards to override individual faces.
     *
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_canonical(const SkinnedMeshSource& canonical);

    /**
     * @brief Bakes one authored mesh onto a single face, replacing whatever was there.
     *
     * @param face      The face to orient onto.
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_side(TileFace face, const SkinnedMeshSource& canonical);

    /** @brief True once every face has geometry -- i.e. a chunk built from this can draw. */
    bool is_baked() const;

    /** @brief Read-only access to one face's baked geometry, for tests and diagnostics. */
    const SideGeometry& side(TileFace face) const {
        return sides_[static_cast<std::size_t>(face)];
    }

    /**
     * @brief Stamps one side into a chunk's merged buffers.
     *
     * The inner loop of chunk meshing: everything expensive (rotation, tangent transport) was
     * done once at bake time, leaving a scale, a translate, a UV remap and an index rebase.
     *
     * Non-uniform `scale` is supported because a tile's vertical step need not equal its
     * horizontal size, and it is handled correctly rather than approximately: positions take
     * the scale, normals and tangents take its component-wise INVERSE (the inverse transpose of
     * a diagonal matrix), then renormalise. Skipping that would tilt the shading of every
     * lateral face the moment height_step and tile_size diverged.
     *
     * @param face      Which baked side to stamp.
     * @param origin    World position of the tile cell's minimum corner.
     * @param scale     Per-axis size of one cell: `{tile_size, tile_size, height_step}`.
     * @param kind      Surface to texture it with; selects the atlas cell.
     * @param out_v     Vertex buffer to append to.
     * @param out_i     Index buffer to append to; rebased onto `out_v`'s current end.
     */
    void append(TileFace face, const glm::vec3& origin, const glm::vec3& scale, TileKind kind,
                std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                bool encode_terrain_uv = false) const;

    /**
     * @brief Appends a mergeable side stretched over `span` tiles/cells (1 on the face's
     *        normal axis), with tile-space UVs encoded for the `terrain` shader -- so the
     *        atlas cell repeats once per tile across the whole quad.
     * @pre side(face).mergeable.
     */
    void append_span(TileFace face, const glm::vec3& origin, const glm::vec3& scale,
                     const glm::vec3& span, TileKind kind,
                     std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i) const;

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
    };

    /** @brief Adds an empty style and returns its index. Kinds map to style 0 until told otherwise. */
    std::uint8_t add_style();

    /** @brief Number of styles; zero means this library only draws voxel sides. */
    std::size_t style_count() const { return styles_.size(); }

    /** @brief True once any style exists -- the gate the chunk mesher switches paths on. */
    bool has_styles() const { return !styles_.empty(); }

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

    /// Fills SideGeometry's mergeable fields: the side must lie in one plane perpendicular to
    /// `face`'s normal axis, every vertex must sit on a corner of the unit square in the other
    /// two axes, and the UVs must be the affine map fitted from three of those corners.
    static void analyse_mergeable_(SideGeometry& side, TileFace face);

    /** @brief One baked geometry per TileFace, indexed by `static_cast<std::size_t>(face)`. */
    std::array<SideGeometry, k_tile_face_count> sides_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
