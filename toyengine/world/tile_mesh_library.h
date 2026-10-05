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

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_types.h>

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

        /// Styled pieces only: per vertex, 1 when its triangle faces up (textured with the
        /// column's top kind), 0 when it is wall (the cell's soil or stone kind). Empty
        /// for the voxel sides, whose kind is chosen per side instead.
        std::vector<std::uint8_t>  surface;

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
    static glm::vec2 encode_uv(TileKind kind, const glm::vec2& local) {
        return glm::vec2(static_cast<float>(kind) * k_uv_cell_stride + 0.5f * k_uv_cell_stride + local.x,
                         local.y);
    }

    /**
     * @brief Bakes one authored mesh onto all six faces.
     *
     * The ordinary case: one `tile_side_flat.yaml` becomes a whole cube's worth of sides.
     * Call bake_side() afterwards to override individual faces.
     *
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_canonical(const SkinnedMeshSource& canonical) {
        for (std::size_t i = 0; i < k_tile_face_count; ++i) {
            bake_side(static_cast<TileFace>(i), canonical);
        }
    }

    /**
     * @brief Bakes one authored mesh onto a single face, replacing whatever was there.
     *
     * @param face      The face to orient onto.
     * @param canonical A side mesh authored in canonical orientation (+Z face of `[0,1]^3`).
     */
    void bake_side(TileFace face, const SkinnedMeshSource& canonical) {
        SideGeometry& out = sides_[static_cast<std::size_t>(face)];
        out.vertices.clear();
        out.indices = canonical.indices;

        const glm::mat4 transform = face_transform(face);
        // A pure rotation, so the inverse transpose is the rotation itself -- normals and
        // tangents take the same matrix as positions, with no renormalisation needed.
        const glm::mat3 rotation(transform);

        out.vertices.reserve(canonical.vertices.size());
        for (const Vertex& v : canonical.vertices) {
            Vertex rotated = v;
            rotated.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
            rotated.normal   = rotation * v.normal;
            rotated.tangent  = glm::vec4(rotation * glm::vec3(v.tangent), v.tangent.w);
            out.vertices.push_back(rotated);
        }
        analyse_mergeable_(out, face);
    }

    /** @brief True once every face has geometry -- i.e. a chunk built from this can draw. */
    bool is_baked() const {
        for (const SideGeometry& side : sides_) {
            if (side.empty()) return false;
        }
        return true;
    }

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
                bool encode_terrain_uv = false) const {
        const SideGeometry& side = sides_[static_cast<std::size_t>(face)];
        if (side.empty()) return;

        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        const glm::vec4 cell = atlas_cell(kind);
        const glm::vec3 normal_scale(1.0f / scale.x, 1.0f / scale.y, 1.0f / scale.z);

        for (const Vertex& v : side.vertices) {
            Vertex placed;
            placed.position = origin + v.position * scale;
            placed.normal   = glm::normalize(v.normal * normal_scale);

            const glm::vec3 tangent = glm::vec3(v.tangent) * scale; // a tangent scales, unlike a normal
            const float tangent_length = glm::length(tangent);
            placed.tangent = glm::vec4(tangent_length > 1e-6f ? tangent / tangent_length
                                                              : glm::vec3(1.0f, 0.0f, 0.0f),
                                       v.tangent.w);

            // encode_terrain_uv: tile-space UVs for a chunk drawn with the `terrain` shader
            // (see encode_uv()) instead of the atlas cell baked into the UV.
            placed.uv = encode_terrain_uv ? encode_uv(kind, v.uv)
                                          : glm::vec2(cell.x + v.uv.x * cell.z, cell.y + v.uv.y * cell.w);
            out_v.push_back(placed);
        }

        for (std::uint32_t index : side.indices) out_i.push_back(base + index);
    }

    /**
     * @brief Appends a mergeable side stretched over `span` tiles/cells (1 on the face's
     *        normal axis), with tile-space UVs encoded for the `terrain` shader -- so the
     *        atlas cell repeats once per tile across the whole quad.
     * @pre side(face).mergeable.
     */
    void append_span(TileFace face, const glm::vec3& origin, const glm::vec3& scale,
                     const glm::vec3& span, TileKind kind,
                     std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i) const {
        const SideGeometry& side = sides_[static_cast<std::size_t>(face)];
        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        for (const Vertex& v : side.vertices) {
            Vertex placed = v;   // flat and axis-aligned: normal and tangent carry over as is
            placed.position = origin + v.position * scale * span;
            const glm::vec2 local = side.uv_origin +
                                    side.uv_du * (v.position[side.axis_u] * span[side.axis_u]) +
                                    side.uv_dv * (v.position[side.axis_v] * span[side.axis_v]);
            placed.uv = encode_uv(kind, local);
            out_v.push_back(placed);
        }
        for (std::uint32_t index : side.indices) out_i.push_back(base + index);
    }

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
    std::uint8_t add_style() {
        styles_.emplace_back();
        bake_fixed_pieces_();
        return static_cast<std::uint8_t>(styles_.size() - 1);
    }

    /** @brief Number of styles; zero means this library only draws voxel sides. */
    std::size_t style_count() const { return styles_.size(); }

    /** @brief True once any style exists -- the gate the chunk mesher switches paths on. */
    bool has_styles() const { return !styles_.empty(); }

    /** @brief Assigns the style a surface kind's cells are shaped with. */
    void set_kind_style(TileKind kind, std::uint8_t style) {
        kind_style_[static_cast<std::size_t>(kind)] = style;
    }

    /** @brief The style a surface kind's cells are shaped with. */
    std::uint8_t style_of(TileKind kind) const {
        const std::uint8_t s = kind_style_[static_cast<std::size_t>(kind)];
        return s < styles_.size() ? s : 0;
    }

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
    void bake_style_piece(std::uint8_t style, TilePiece piece, const SkinnedMeshSource& source) {
        StyleSet& set = styles_[style];
        const std::size_t p = static_cast<std::size_t>(piece);
        const std::vector<Vertex> soup = triangle_soup_(source);
        bake_oriented_(soup, set.pieces[p]);
        set.authored[p] = true;
        for (auto& plane : set.sections[p]) for (auto& g : plane) g = SideGeometry{};

        if (piece == TilePiece::WallCapContinue || piece == TilePiece::WallContinue) {
            // Straight halves are extrusions along x, so x in [0.5,1] can be remapped to [0,1]
            // and then stretched freely: a straight run of any length is one strip.
            std::vector<Vertex> unit = soup;
            for (Vertex& v : unit) v.position.x = (v.position.x - 0.5f) * 2.0f;
            Oriented turned;
            bake_oriented_(unit, turned);
            for (std::size_t q = 0; q < 4; ++q) set.straight[piece == TilePiece::WallCapContinue ? 1 : 0][q] = turned[q];
        }

        const bool wall = piece >= TilePiece::WallCapContinue && piece <= TilePiece::WallConcave;
        if (wall) {
            const WallEnd end = static_cast<WallEnd>(
                (static_cast<std::uint8_t>(piece) - static_cast<std::uint8_t>(TilePiece::WallCapContinue)) % 3u);
            const bool cap = piece <= TilePiece::WallCapConcave;
            const bool concave = end == WallEnd::Concave;
            for (int top = 0; top < 2; ++top) {
                if (top && cap) continue; // a cap is the top of its wall; nothing sits on it
                const float z = top ? 1.0f : 0.0f;
                const glm::vec3 corner(1.0f, 1.0f, z);
                const glm::vec3 start(0.5f, 1.0f, z);
                bake_oriented_(derive_section_(soup, glm::vec3(0.0f, 0.0f, z),
                                               glm::vec3(0.0f, 0.0f, top ? 1.0f : -1.0f),
                                               glm::vec3(0.5f, 0.5f, z), &start, concave ? &corner : nullptr),
                               set.sections[p][static_cast<std::size_t>(top ? SectionPlane::Top : SectionPlane::Bottom)]);
            }
            Oriented& end_section = set.sections[p][static_cast<std::size_t>(SectionPlane::End)];
            if (end == WallEnd::Continue) {
                const glm::vec3 a(1.0f, 0.5f, 0.0f), b(1.0f, 0.5f, 1.0f);
                bake_oriented_(derive_section_(soup, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                               glm::vec3(1.0f, 0.5f, 0.5f), &a, &b),
                               end_section);
            } else if (end == WallEnd::Concave) {
                const glm::vec3 a(1.0f, 1.0f, 0.0f), b(1.0f, 1.0f, 1.0f);
                bake_oriented_(derive_section_(soup, glm::vec3(1.0f, 1.0f, 0.0f),
                                               glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)),
                                               glm::vec3(1.0f, 1.0f, 0.5f), &a, &b),
                               end_section);
            }
        }
    }

    /**
     * @brief Fills every piece a style did not author from its nearest relative, so a partial
     *        style degrades to simpler shapes rather than punching holes in the world.
     *
     * Cap tiers fall back to their plain tier, plain corners to the plain straight wall, the
     * corner quadrants to the edge quadrant and that to the inner one.
     */
    void finish_styles() {
        static const std::pair<TilePiece, TilePiece> fallbacks[] = {
            {TilePiece::TopEdge, TilePiece::TopInner},       {TilePiece::TopOuter, TilePiece::TopEdge},
            {TilePiece::WallConvex, TilePiece::WallContinue}, {TilePiece::WallConcave, TilePiece::WallContinue},
            {TilePiece::WallCapContinue, TilePiece::WallContinue}, {TilePiece::WallCapConvex, TilePiece::WallConvex},
            {TilePiece::WallCapConcave, TilePiece::WallConcave}};
        for (StyleSet& set : styles_) {
            for (const auto& [piece, from] : fallbacks) {
                const std::size_t p = static_cast<std::size_t>(piece);
                const std::size_t f = static_cast<std::size_t>(from);
                if (set.authored[p] || set.pieces[f][0].empty()) continue;
                set.pieces[p]   = set.pieces[f];
                set.sections[p] = set.sections[f];
                if (piece == TilePiece::WallCapContinue) set.straight[1] = set.straight[0];
            }
        }
    }

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
                                std::uint8_t orientation) const {
        return styles_[style].sections[static_cast<std::size_t>(piece)][static_cast<std::size_t>(plane)]
                                      [orientation & 7u];
    }

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
     * Each vertex takes `top_kind` or `wall_kind` by its baked surface class. A `top_anchored`
     * piece -- the cap tier -- keeps everything above its z = 0.5 at tile proportions, pinned
     * to the cell top, and stretches only the band below to fill the step: a lip authored for a
     * cubic cell therefore stays round on a tall step instead of turning into an ellipse.
     * Steps shorter than half a tile fall back to plain linear scaling.
     *
     * @param geometry     What to stamp.
     * @param origin       World position of the cell's minimum corner.
     * @param scale        `{tile_size, tile_size, height_step}`.
     * @param top_kind     Atlas kind of up-facing triangles.
     * @param wall_kind    Atlas kind of the rest.
     * @param top_anchored Use the cap tier's anchored vertical mapping.
     * @param out_v        Vertex buffer to append to.
     * @param out_i        Index buffer to append to; rebased onto `out_v`'s current end.
     * @param stretch_axis World axis (0 = x, 1 = y) `stretch` lengthens.
     * @param stretch      Length multiplier along `stretch_axis`, for a straight() run; only
     *                     valid for geometry invariant along that axis.
     */
    void append_styled(const SideGeometry& geometry, const glm::vec3& origin, const glm::vec3& scale,
                       TileKind top_kind, TileKind wall_kind, bool top_anchored,
                       std::vector<Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                       int stretch_axis = 0, float stretch = 1.0f) const {
        if (geometry.empty()) return;
        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        const glm::vec4 top_cell  = atlas_cell(top_kind);
        const glm::vec4 wall_cell = atlas_cell(wall_kind);
        const float tile   = scale.x;
        const float height = scale.z;
        const bool anchored = top_anchored && height >= 0.5f * tile;
        const float lower   = std::max((height - 0.5f * tile) / 0.5f, 1e-3f);

        // No reserve(size + n) here: called thousands of times per chunk, an exact-size reserve
        // defeats the vector's geometric growth and turns the whole mesh quadratic.
        for (std::size_t i = 0; i < geometry.vertices.size(); ++i) {
            const Vertex& v = geometry.vertices[i];
            glm::vec3 s = scale;
            float z = v.position.z * height;
            if (anchored) {
                if (v.position.z >= 0.5f) {
                    z = height - (1.0f - v.position.z) * tile;
                    s = glm::vec3(tile);
                } else {
                    z = v.position.z * lower;
                    s = glm::vec3(tile, tile, lower);
                }
            }
            Vertex placed;
            glm::vec3 local(v.position.x * tile, v.position.y * scale.y, z);
            local[stretch_axis] *= stretch;
            placed.position = origin + local;
            placed.normal   = glm::normalize(v.normal / s);
            // Any tangent orthogonal to the normal, chosen from the normal alone: nothing samples
            // a normal map here, and a deterministic tangent lets equal corners weld.
            const glm::vec3 axis = std::abs(placed.normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                    : glm::vec3(1.0f, 0.0f, 0.0f);
            placed.tangent = glm::vec4(glm::normalize(glm::cross(axis, placed.normal)), 1.0f);
            // The kind's cell centre: terrain_styled.frag reads the kind from the cell and draws
            // the detail procedurally, and a constant UV per kind lets corners weld.
            const glm::vec4& cell = (i < geometry.surface.size() && geometry.surface[i]) ? top_cell : wall_cell;
            placed.uv = glm::vec2(cell.x + 0.5f * cell.z, cell.y + 0.5f * cell.w);
            out_v.push_back(placed);
        }
        for (std::uint32_t index : geometry.indices) out_i.push_back(base + index);
    }

private:
    /// A mesh source flattened to a plain triangle list (three vertices per triangle).
    static std::vector<Vertex> triangle_soup_(const SkinnedMeshSource& source) {
        std::vector<Vertex> soup;
        soup.reserve(source.indices.size());
        for (std::uint32_t index : source.indices) {
            if (index < source.vertices.size()) soup.push_back(source.vertices[index]);
        }
        soup.resize(soup.size() - soup.size() % 3);
        return soup;
    }

    /**
     * Orients a canonical triangle soup eight ways, classing each triangle as up-facing (top
     * kind) or wall from its oriented geometric normal. Mirrored orientations reverse the
     * winding. UVs and tangents are not baked: append_styled() writes the kind's cell centre and
     * a normal-derived tangent, because terrain_styled.frag needs nothing else.
     */
    static void bake_oriented_(const std::vector<Vertex>& soup, Oriented& out) {
        for (std::uint8_t o = 0; o < k_tile_orientation_count; ++o) {
            SideGeometry& g = out[o];
            g = SideGeometry{};
            const glm::mat4& m = variant_transform(o);
            const glm::mat3 r(m);
            const bool mirrored = orientation_mirrors(o);
            g.vertices.reserve(soup.size());
            for (std::size_t t = 0; t + 2 < soup.size(); t += 3) {
                Vertex v[3];
                for (int k = 0; k < 3; ++k) {
                    v[k] = soup[t + static_cast<std::size_t>(k)];
                    v[k].position = glm::vec3(m * glm::vec4(v[k].position, 1.0f));
                    const glm::vec3 n = r * v[k].normal;
                    v[k].normal = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
                }
                if (mirrored) std::swap(v[1], v[2]);
                const glm::vec3 cross = glm::cross(v[1].position - v[0].position, v[2].position - v[0].position);
                const float area = glm::length(cross);
                if (area < 1e-9f) continue;
                const bool up = cross.z / area > 0.5f;
                const std::uint32_t base = static_cast<std::uint32_t>(g.vertices.size());
                for (const Vertex& vert : v) {
                    g.vertices.push_back(vert);
                    g.surface.push_back(up ? 1u : 0u);
                }
                g.indices.push_back(base);
                g.indices.push_back(base + 1);
                g.indices.push_back(base + 2);
            }
        }
    }

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
                                               const glm::vec3* anchor_start, const glm::vec3* anchor_end) {
        std::vector<glm::vec3> polygon;
        if (anchor_start) polygon.push_back(*anchor_start);
        const std::vector<glm::vec3> chain = boundary_chain_(soup, point, normal, anchor_start ? *anchor_start : ref);
        polygon.insert(polygon.end(), chain.begin(), chain.end());
        if (anchor_end) polygon.push_back(*anchor_end);
        return fan_(polygon, ref, normal);
    }

    /**
     * The piece's boundary edges lying on a plane, as one ordered polyline: chained, then
     * concatenated greedily by nearest endpoint starting from `start` (gaps bridged straight).
     */
    static std::vector<glm::vec3> boundary_chain_(const std::vector<Vertex>& soup, const glm::vec3& point,
                                                  const glm::vec3& normal, const glm::vec3& start) {
        constexpr float k_eps = 1e-3f;
        std::vector<glm::vec3> welded;
        std::map<std::array<long, 3>, std::uint32_t> ids;
        auto id_of = [&](const glm::vec3& p) {
            const std::array<long, 3> key = {std::lround(p.x * 1e4f), std::lround(p.y * 1e4f), std::lround(p.z * 1e4f)};
            auto it = ids.find(key);
            if (it != ids.end()) return it->second;
            const std::uint32_t id = static_cast<std::uint32_t>(welded.size());
            welded.push_back(p);
            ids.emplace(key, id);
            return id;
        };
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edge_use;
        for (std::size_t t = 0; t + 2 < soup.size(); t += 3) {
            const std::uint32_t a = id_of(soup[t].position);
            const std::uint32_t b = id_of(soup[t + 1].position);
            const std::uint32_t c = id_of(soup[t + 2].position);
            for (auto [x, y] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                if (x == y) continue;
                ++edge_use[{std::min(x, y), std::max(x, y)}];
            }
        }
        auto on_plane = [&](std::uint32_t id) { return std::abs(glm::dot(welded[id] - point, normal)) < k_eps; };
        std::map<std::uint32_t, std::vector<std::uint32_t>> adjacency;
        for (const auto& [edge, uses] : edge_use) {
            if (uses != 1 || !on_plane(edge.first) || !on_plane(edge.second)) continue;
            adjacency[edge.first].push_back(edge.second);
            adjacency[edge.second].push_back(edge.first);
        }

        // Chains: walk from each unvisited endpoint (degree 1), then any leftover loops.
        std::vector<std::vector<glm::vec3>> chains;
        std::map<std::uint32_t, bool> visited;
        auto walk = [&](std::uint32_t from) {
            std::vector<glm::vec3> chain;
            std::uint32_t current = from;
            std::uint32_t previous = UINT32_MAX;
            while (true) {
                visited[current] = true;
                chain.push_back(welded[current]);
                std::uint32_t next = UINT32_MAX;
                for (std::uint32_t n : adjacency[current]) {
                    if (n != previous && !visited[n]) { next = n; break; }
                }
                if (next == UINT32_MAX) break;
                previous = current;
                current = next;
            }
            chains.push_back(std::move(chain));
        };
        for (const auto& [id, nbrs] : adjacency) if (nbrs.size() == 1 && !visited[id]) walk(id);
        for (const auto& [id, nbrs] : adjacency) if (!visited[id]) walk(id);

        // Concatenate greedily by nearest endpoint, starting from `start`.
        std::vector<glm::vec3> polygon;
        glm::vec3 cursor = start;
        std::vector<bool> used(chains.size(), false);
        for (std::size_t n = 0; n < chains.size(); ++n) {
            std::size_t best = SIZE_MAX;
            bool reversed = false;
            float best_d = 1e30f;
            for (std::size_t c = 0; c < chains.size(); ++c) {
                if (used[c]) continue;
                const float df = glm::length(chains[c].front() - cursor);
                const float db = glm::length(chains[c].back() - cursor);
                if (df < best_d) { best_d = df; best = c; reversed = false; }
                if (db < best_d) { best_d = db; best = c; reversed = true; }
            }
            used[best] = true;
            std::vector<glm::vec3>& chain = chains[best];
            if (reversed) std::reverse(chain.begin(), chain.end());
            polygon.insert(polygon.end(), chain.begin(), chain.end());
            cursor = chain.back();
        }
        return polygon;
    }

    /// Fans a polygon from `ref` into a triangle soup facing `normal`, skipping slivers.
    static std::vector<Vertex> fan_(const std::vector<glm::vec3>& polygon, const glm::vec3& ref,
                                    const glm::vec3& normal) {
        std::vector<Vertex> out;
        for (std::size_t i = 0; i + 1 < polygon.size(); ++i) {
            glm::vec3 a = polygon[i];
            glm::vec3 b = polygon[i + 1];
            const glm::vec3 cross = glm::cross(a - ref, b - ref);
            if (glm::length(cross) < 1e-7f) continue;
            if (glm::dot(cross, normal) < 0.0f) std::swap(a, b);
            for (const glm::vec3& p : {ref, a, b}) {
                Vertex v;
                v.position = p;
                v.normal   = normal;
                v.uv       = glm::vec2(0.0f);
                v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                out.push_back(v);
            }
        }
        return out;
    }

    /// The style-independent pieces: the whole flat top and the convex foot.
    void bake_fixed_pieces_() {
        if (!full_top_.empty()) return;
        auto vertex = [](const glm::vec3& p) {
            Vertex v;
            v.position = p;
            v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
            v.uv       = glm::vec2(p.x, p.y);
            v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            return v;
        };
        const std::vector<Vertex> quad = {vertex({0, 0, 1}), vertex({1, 0, 1}), vertex({1, 1, 1}),
                                          vertex({0, 0, 1}), vertex({1, 1, 1}), vertex({0, 1, 1})};
        Oriented quads;
        bake_oriented_(quad, quads);
        full_top_ = quads[0];
        const std::vector<Vertex> foot = {vertex({0.5f, 0.5f, 0}), vertex({1, 1, 0}), vertex({0.5f, 1, 0})};
        bake_oriented_(foot, foot_);
    }

    std::vector<StyleSet>                     styles_;
    std::array<std::uint8_t, k_tile_kind_count> kind_style_{};
    SideGeometry                              full_top_;
    Oriented                                  foot_;

    /// Fills SideGeometry's mergeable fields: the side must lie in one plane perpendicular to
    /// `face`'s normal axis, every vertex must sit on a corner of the unit square in the other
    /// two axes, and the UVs must be the affine map fitted from three of those corners.
    static void analyse_mergeable_(SideGeometry& side, TileFace face) {
        side.mergeable = false;
        if (side.empty()) return;
        const glm::vec3 n = face_normal(face);
        const int axis_n = (std::abs(n.x) > 0.5f) ? 0 : (std::abs(n.y) > 0.5f) ? 1 : 2;
        side.axis_u = (axis_n == 0) ? 1 : 0;
        side.axis_v = (axis_n == 2) ? 1 : 2;
        const float eps = 1e-4f;
        auto snap = [eps](float x, int& out) {
            if (std::abs(x) < eps)        { out = 0; return true; }
            if (std::abs(x - 1.0f) < eps) { out = 1; return true; }
            return false;
        };
        const Vertex* corner[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
        const float plane = side.vertices[0].position[axis_n];
        for (const Vertex& v : side.vertices) {
            int pu = 0, pv = 0;
            if (std::abs(v.position[axis_n] - plane) > eps) return;
            if (!snap(v.position[side.axis_u], pu) || !snap(v.position[side.axis_v], pv)) return;
            corner[pu][pv] = &v;
        }
        if (!corner[0][0] || !corner[1][0] || !corner[0][1]) return;
        side.uv_origin = corner[0][0]->uv;
        side.uv_du     = corner[1][0]->uv - side.uv_origin;
        side.uv_dv     = corner[0][1]->uv - side.uv_origin;
        for (const Vertex& v : side.vertices) {
            const glm::vec2 fit = side.uv_origin + side.uv_du * v.position[side.axis_u] +
                                  side.uv_dv * v.position[side.axis_v];
            if (glm::length(fit - v.uv) > 1e-3f) return;
        }
        side.mergeable = true;
    }

    /** @brief One baked geometry per TileFace, indexed by `static_cast<std::size_t>(face)`. */
    std::array<SideGeometry, k_tile_face_count> sides_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_MESH_LIBRARY_H
