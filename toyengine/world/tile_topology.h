/**
 * @file tile_topology.h
 * @brief Neighbourhood classification for styled terrain tiles: which authored piece goes where,
 *        and at which of the eight D4 orientations.
 *
 * The voxel mesher (terrain_chunk.h) stamps one shape per exposed side and never looks further
 * than the neighbour across that side. That is enough for blocks and not enough for the Animal
 * Crossing read: a rounded lip belongs only on an exposed edge, a quarter-round only at a convex
 * corner, a fillet only at a concave one. Putting a rounded shape on every tile quilts a plateau
 * into buns. This file is the classifier that prevents that -- pure functions from a column's
 * neighbourhood to a piece and an orientation, depending on nothing but tile_types.h.
 *
 * ### The vocabulary
 *
 * A styled tile is assembled from **pieces**, each authored once in one canonical corner of the
 * unit cell and placed by variant_transform():
 *
 * - **Tops** are four quadrants. A quadrant's shape depends only on whether its two cardinal
 *   neighbours are lower (exposed): neither -> `TopInner`, one -> `TopEdge`, both -> `TopOuter`.
 *   Three shapes cover every one of the 47 blob-autotile cases. Authored as the +X+Y quadrant
 *   `[0.5,1]^2`, with the +Y edge the exposed one.
 * - **Walls** are two halves per exposed cell. A half's shape depends on the state of the end it
 *   reaches (WallEnd): the cliff continues straight, wraps around a convex corner, or turns into
 *   a concave one. The topmost cell of a wall is the **cap** tier, which carries the lip's
 *   round-over; cells below are plain vertical extrusions. Authored as the right half
 *   (`x in [0.5,1]`) of the +Y face, with the end state at `x = 1`.
 *
 * ### Concave corners
 *
 * A concave fillet is material added into the LOWER cell's corner, with the arc centred at
 * `(1 - r, 1 + r)` in the canonical wall frame, so the cliff curves rather than meeting at a
 * right angle. The two walls meeting there each own a 45-degree half of it, which is why a
 * concave half never needs geometry past `x = 1`.
 */

#ifndef TOYENGINE_WORLD_TILE_TOPOLOGY_H
#define TOYENGINE_WORLD_TILE_TOPOLOGY_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

/**
 * @struct ColumnPad
 * @brief A chunk's columns plus a one-tile skirt of its neighbours', in local chunk coordinates.
 *
 * Indexed by local tile coordinates running `[-1, chunk_size]` on both axes: `at(-1, 0)` is the
 * neighbouring chunk's edge column, `at(0, 0)` this chunk's corner.
 */
struct ColumnPad {
    std::int32_t            chunk_size = 0;
    std::vector<TileColumn> columns;

    /** @brief Allocates the pad for a chunk of `size` tiles per edge. */
    void resize(std::int32_t size) {
        chunk_size = size;
        const std::size_t stride = static_cast<std::size_t>(size) + 2;
        columns.assign(stride * stride, TileColumn{});
    }

    /** @brief Mutable access by local tile coordinate, valid over `[-1, chunk_size]`. */
    TileColumn& at(std::int32_t local_x, std::int32_t local_y) {
        return columns[index_(local_x, local_y)];
    }

    /** @brief Read-only access by local tile coordinate, valid over `[-1, chunk_size]`. */
    const TileColumn& at(std::int32_t local_x, std::int32_t local_y) const {
        return columns[index_(local_x, local_y)];
    }

    /** @brief Read-only access by a local coordinate vector. */
    const TileColumn& at(const glm::ivec2& p) const { return at(p.x, p.y); }

private:
    std::size_t index_(std::int32_t local_x, std::int32_t local_y) const {
        const std::int32_t stride = chunk_size + 2;
        const std::int32_t cx = std::clamp(local_x + 1, 0, stride - 1);
        const std::int32_t cy = std::clamp(local_y + 1, 0, stride - 1);
        return static_cast<std::size_t>(cy) * static_cast<std::size_t>(stride) +
               static_cast<std::size_t>(cx);
    }
};

/**
 * @enum TilePiece
 * @brief The authored shapes a tile style is made of. See this file's doc for the frames.
 *
 * The order is the order of a style's piece table and of tile_piece_name().
 */
enum class TilePiece : std::uint8_t {
    TopInner,        /**< @brief A quadrant with no exposed edge: flat. */
    TopEdge,         /**< @brief A quadrant with its +Y edge exposed: inset by the lip. */
    TopOuter,        /**< @brief A quadrant with both edges exposed: corner rounded. */
    WallCapContinue, /**< @brief Top wall cell, straight end: the lip's round-over. */
    WallCapConvex,   /**< @brief Top wall cell, wrapping an outer corner. */
    WallCapConcave,  /**< @brief Top wall cell, filleting an inner corner (with its top fill). */
    WallContinue,    /**< @brief Lower wall cell, straight end. */
    WallConvex,      /**< @brief Lower wall cell, outer corner. */
    WallConcave      /**< @brief Lower wall cell, inner corner. */
};

/** @brief Number of distinct `TilePiece` values. */
inline constexpr std::size_t k_tile_piece_count = 9;

/**
 * @brief The suffix a piece's mesh file carries: a style `tile_round` loads
 *        `meshes/tile_round_<suffix>.yaml`.
 *
 * The one table the scene parser expands styles through, so a file name and the piece it is
 * baked as can never disagree.
 */
inline const char* tile_piece_name(TilePiece piece) {
    static const char* const names[k_tile_piece_count] = {
        "top_inner", "top_edge", "top_outer",
        "wall_cap_continue", "wall_cap_convex", "wall_cap_concave",
        "wall_continue", "wall_convex", "wall_concave"};
    return names[static_cast<std::size_t>(piece)];
}

/**
 * @enum WallEnd
 * @brief What a wall does at one of its two ends, at one cell of its height.
 *
 * From the along-wall neighbour `a` and the diagonal `d = a + face`, at the cell being emitted:
 *
 * | condition                          | state      |
 * |---|---|
 * | `a.steps <= cell`                  | `Convex`   -- the cliff wraps outward around our corner |
 * | `a.steps > cell`, `d.steps <= cell`| `Continue` -- the cliff runs straight on along `a` |
 * | `a.steps > cell`, `d.steps > cell` | `Concave`  -- the cliff turns inward along `d`'s side |
 *
 * Classified per cell, not per column: a tall wall's ends genuinely change with depth.
 */
enum class WallEnd : std::uint8_t { Continue, Convex, Concave };

/** @brief The wall piece for a tier and an end state. */
inline TilePiece wall_piece(bool cap, WallEnd end) {
    const std::uint8_t base = cap ? static_cast<std::uint8_t>(TilePiece::WallCapContinue)
                                  : static_cast<std::uint8_t>(TilePiece::WallContinue);
    return static_cast<TilePiece>(base + static_cast<std::uint8_t>(end));
}

/** @brief `v` turned `quarter_turns` x 90 degrees counter-clockwise about +Z. */
inline glm::ivec2 rotate_ccw(const glm::ivec2& v, std::uint8_t quarter_turns) {
    glm::ivec2 out = v;
    for (std::uint8_t i = 0; i < (quarter_turns & 3u); ++i) out = glm::ivec2(-out.y, out.x);
    return out;
}

/** @brief The quarter turns carrying the canonical wall face (+Y, North) onto `face`. */
inline std::uint8_t lateral_quarter_turns(TileFace face) {
    switch (face) {
        case TileFace::North: return 0;
        case TileFace::West:  return 1;
        case TileFace::South: return 2;
        case TileFace::East:  return 3;
        default:              return 0;
    }
}

/** @brief The lateral face whose outward step is `step` (a unit cardinal). */
inline TileFace face_of_step(const glm::ivec2& step) {
    if (step.y > 0) return TileFace::North;
    if (step.y < 0) return TileFace::South;
    if (step.x > 0) return TileFace::East;
    return TileFace::West;
}

/**
 * @struct PiecePlacement
 * @brief A piece and the D4 orientation it is stamped at.
 */
struct PiecePlacement {
    TilePiece    piece       = TilePiece::TopInner;
    std::uint8_t orientation = 0;
};

/**
 * @brief The top piece for one quadrant of a tile.
 *
 * Quadrant `q` is the canonical +X+Y quadrant turned `q` quarter turns counter-clockwise, so its
 * two cardinals are `rotate_ccw(+Y, q)` and `rotate_ccw(+X, q)`. The canonical TopEdge has its
 * +Y edge exposed; the other chirality is the mirror (which keeps +Y, moving the quadrant to
 * -X+Y) turned one quarter less.
 *
 * @param q           Quadrant, `0..3`.
 * @param exposed_y   The cardinal `rotate_ccw(+Y, q)` is lower than this column.
 * @param exposed_x   The cardinal `rotate_ccw(+X, q)` is lower than this column.
 */
inline PiecePlacement classify_quadrant(std::uint8_t q, bool exposed_y, bool exposed_x) {
    q &= 3u;
    if (exposed_y && exposed_x) return {TilePiece::TopOuter, q};
    if (exposed_y)              return {TilePiece::TopEdge, q};
    if (exposed_x)              return {TilePiece::TopEdge, static_cast<std::uint8_t>(4u | ((q + 3u) & 3u))};
    return {TilePiece::TopInner, q};
}

/**
 * @brief The orientation placing the canonical wall half on `face`, reaching toward `right`
 *        (`true`: the end at `rotate_ccw(+X, turns)`) or the other end.
 */
inline std::uint8_t wall_orientation(TileFace face, bool right) {
    const std::uint8_t turns = lateral_quarter_turns(face);
    return right ? turns : static_cast<std::uint8_t>(4u | turns);
}

/** @brief The along-wall direction a wall half on `face` reaches toward. */
inline glm::ivec2 wall_end_direction(TileFace face, bool right) {
    return rotate_ccw(glm::ivec2(right ? 1 : -1, 0), lateral_quarter_turns(face));
}

/**
 * @brief Classifies one end of one wall cell. See WallEnd for the table.
 *
 * @param pad    The columns.
 * @param column Local coordinate of the wall's own column.
 * @param face   The wall's face.
 * @param along  The end's along-wall direction (wall_end_direction()).
 * @param cell   The cell index being emitted.
 */
inline WallEnd classify_wall_end(const ColumnPad& pad, const glm::ivec2& column, TileFace face,
                                 const glm::ivec2& along, std::int32_t cell) {
    const glm::ivec2 a = column + along;
    if (pad.at(a).steps <= cell) return WallEnd::Convex;
    const glm::ivec2 diag = a + face_step(face);
    if (pad.at(diag).steps <= cell) return WallEnd::Continue;
    return WallEnd::Concave;
}

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_TOPOLOGY_H
