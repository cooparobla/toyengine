/**
 * @file tile_types.h
 * @brief The vocabulary of the terrain tile system: faces, surface kinds, columns and chunk
 *        coordinates, plus the canonical-face-to-target-face transforms.
 *
 * A terrain **tile** is one cell of the world grid, extruded from z = 0 up to its own height.
 * A tile's visible geometry is its **sides**: a top, a bottom, and four laterals. Each side is
 * an authored mesh (see tile_mesh_library.h), and a tile contributes a side only where that
 * side is **exposed** -- a top always, a lateral only against a lower neighbour. The chunk
 * mesher (terrain_chunk.h) merges every exposed side in a chunk into one GPU mesh.
 *
 * The key geometric convention lives here: every side mesh is authored ONCE, as the **+Z face
 * of a unit cube spanning [0,1]^3** (matching assets/scenes/pixel_demo/meshes/cube.000.yaml's
 * own [0,1]^3 convention), and every other face is that same mesh rotated about the cube's
 * centre by face_transform(). That is what lets a smoother tile -- a chamfered rim, a sloped
 * cap, a curved patch -- be dropped in as a data change: the mesher never learns what shape a
 * side is, only where it goes.
 *
 * Z is up in this engine (see assets/config.yaml's `gravity: {0, 0, -9.81}`), so mapcoopa's
 * grid (x, y) maps straight onto world (x, y) and elevation onto world z.
 */

#ifndef TOYENGINE_WORLD_TILE_TYPES_H
#define TOYENGINE_WORLD_TILE_TYPES_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace toy {
namespace world {

/**
 * @enum TileFace
 * @brief The six sides of a tile, named by the axis each one faces.
 *
 * The order is the order face_transform() indexes, and `Top` is deliberately first: it is the
 * canonical authoring orientation every other face is derived from, and the only side a
 * flat-ground tile ever emits.
 */
enum class TileFace : std::uint8_t {
    Top,    /**< @brief +Z, the canonical authoring orientation. */
    Bottom, /**< @brief -Z; only emitted when TerrainParams::emit_bottom is set. */
    North,  /**< @brief +Y. */
    South,  /**< @brief -Y. */
    East,   /**< @brief +X. */
    West    /**< @brief -X. */
};

/** @brief Number of distinct `TileFace` values; the size of any face-indexed table. */
inline constexpr std::size_t k_tile_face_count = 6;

/** @brief The four lateral faces, in the order the chunk mesher tests them. */
inline constexpr std::array<TileFace, 4> k_lateral_faces = {
    TileFace::North, TileFace::South, TileFace::East, TileFace::West};

/**
 * @enum TileKind
 * @brief The surface a face is textured with -- one cell of the terrain atlas.
 *
 * A *surface*, not a biome: thirty-three biomes (see coopa::maps::Biome) collapse onto this
 * much smaller set, because what a face needs from a biome is only which patch of the atlas to
 * sample. TerrainSampler owns that collapse; nothing else in the tile system knows biomes exist.
 *
 * Appended, never reordered: a kind's position IS its atlas cell (see atlas_cell()), so
 * inserting above would silently re-texture the whole world.
 */
enum class TileKind : std::uint8_t {
    Grass,
    Dirt,
    Stone,
    Sand,
    Snow,
    Rock,
    Water,
    Ice,
    Moss,
    Clay,
    Ash,
    Salt
};

/** @brief Number of distinct `TileKind` values; the number of occupied atlas cells. */
inline constexpr std::size_t k_tile_kind_count = 12;

/** @brief Atlas cells per row. 4 x 4 leaves four spare cells for new kinds without a re-layout. */
inline constexpr std::uint32_t k_atlas_columns = 4;
/** @brief Atlas cells per column. */
inline constexpr std::uint32_t k_atlas_rows = 4;
/** @brief Edge length of one atlas cell, in texels; see tools/gen_terrain_atlas.py. */
inline constexpr std::uint32_t k_atlas_cell_texels = 16;

/**
 * @brief The UV rectangle a `TileKind` occupies in the terrain atlas.
 *
 * Returned as `{u0, v0, du, dv}` -- an origin plus a size, so a caller maps a side mesh's own
 * `[0,1]` UV into it with one multiply-add.
 *
 * The rectangle is inset by half a texel on every edge. The atlas is NEAREST-sampled (this is a
 * pixel-art engine -- see toy::loaders::PixelTextureLoader), and without the inset a UV landing
 * exactly on a cell boundary can round into the neighbouring cell and paint a one-texel stripe
 * of the wrong surface along every tile edge.
 *
 * @param kind The surface to look up.
 * @return `{u0, v0, du, dv}` in normalised texture coordinates.
 */
inline glm::vec4 atlas_cell(TileKind kind) {
    const std::uint32_t index  = static_cast<std::uint32_t>(kind);
    const std::uint32_t column = index % k_atlas_columns;
    const std::uint32_t row    = index / k_atlas_columns;

    const float cell_u = 1.0f / static_cast<float>(k_atlas_columns);
    const float cell_v = 1.0f / static_cast<float>(k_atlas_rows);
    const float inset_u = 0.5f / static_cast<float>(k_atlas_columns * k_atlas_cell_texels);
    const float inset_v = 0.5f / static_cast<float>(k_atlas_rows * k_atlas_cell_texels);

    return glm::vec4(static_cast<float>(column) * cell_u + inset_u,
                     static_cast<float>(row) * cell_v + inset_v,
                     cell_u - 2.0f * inset_u,
                     cell_v - 2.0f * inset_v);
}

/**
 * @struct TileColumn
 * @brief One tile's sampled state: how tall it is and what it is made of.
 *
 * `steps` is a COUNT of vertical tile steps, not a height in world units -- the quantisation is
 * what gives the terraced, block-stacked read, and keeping it integral is what lets the mesher
 * decide face exposure with an exact comparison rather than an epsilon one.
 */
struct TileColumn {
    std::int32_t steps     = 0;                 /**< @brief Height in whole tile steps above z = 0. */
    TileKind     top_kind  = TileKind::Grass;   /**< @brief Surface of the top face. */
    TileKind     side_kind = TileKind::Dirt;    /**< @brief Surface of the lateral faces just under the top. */
    bool         water     = false;             /**< @brief This column is ocean, lake or river surface. */
};

/**
 * @struct ChunkCoord
 * @brief Integer address of a chunk in the chunk grid, in units of TerrainParams::chunk_size tiles.
 */
struct ChunkCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const ChunkCoord& other) const { return x == other.x && y == other.y; }
    bool operator!=(const ChunkCoord& other) const { return !(*this == other); }
};

/**
 * @brief Chebyshev (chessboard) distance between two chunks, in chunks.
 *
 * Chebyshev rather than Euclidean because the loaded region is a square ring around the camera:
 * `chunk_distance(c, centre) <= view_radius` is exactly "inside the square of side
 * 2*view_radius + 1", which is the set the streamer builds.
 *
 * @param a One chunk.
 * @param b The other.
 * @return The larger of the two axis distances.
 */
inline std::int32_t chunk_distance(const ChunkCoord& a, const ChunkCoord& b) {
    const std::int32_t dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const std::int32_t dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

/**
 * @brief The rigid transform taking the canonical (+Z, `[0,1]^3` unit cube) side mesh onto `face`.
 *
 * A rotation about the cube's own centre `(0.5, 0.5, 0.5)`, so the result still spans `[0,1]^3`
 * and can be translated by a tile's origin directly. Built once into a function-local static
 * table: glm's matrix constructors are not constexpr, and this is read once per emitted face.
 *
 * @param face The face to orient onto.
 * @return The object-space transform; multiply a canonical position by it.
 */
inline const glm::mat4& face_transform(TileFace face) {
    static const std::array<glm::mat4, k_tile_face_count> table = [] {
        const glm::vec3 centre(0.5f, 0.5f, 0.5f);
        const glm::vec3 axis_x(1.0f, 0.0f, 0.0f);
        const glm::vec3 axis_y(0.0f, 1.0f, 0.0f);

        // Each entry is translate(centre) * rotate * translate(-centre): spin the canonical +Z
        // quad about the cube's middle so it lands on the named face, still inside [0,1]^3.
        auto about_centre = [&](float degrees, const glm::vec3& axis) {
            return glm::translate(glm::mat4(1.0f), centre) *
                   glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis) *
                   glm::translate(glm::mat4(1.0f), -centre);
        };

        std::array<glm::mat4, k_tile_face_count> out{};
        out[static_cast<std::size_t>(TileFace::Top)]    = glm::mat4(1.0f);
        out[static_cast<std::size_t>(TileFace::Bottom)] = about_centre(180.0f, axis_x);
        out[static_cast<std::size_t>(TileFace::North)]  = about_centre(-90.0f, axis_x);
        out[static_cast<std::size_t>(TileFace::South)]  = about_centre(90.0f, axis_x);
        out[static_cast<std::size_t>(TileFace::East)]   = about_centre(90.0f, axis_y);
        out[static_cast<std::size_t>(TileFace::West)]   = about_centre(-90.0f, axis_y);
        return out;
    }();
    return table[static_cast<std::size_t>(face)];
}

/**
 * @brief The unit outward normal of `face`, in tile space.
 *
 * Stated directly rather than derived from face_transform() so a test can check the two agree.
 *
 * @param face The face.
 * @return Its outward direction.
 */
inline glm::vec3 face_normal(TileFace face) {
    switch (face) {
        case TileFace::Top:    return glm::vec3(0.0f, 0.0f, 1.0f);
        case TileFace::Bottom: return glm::vec3(0.0f, 0.0f, -1.0f);
        case TileFace::North:  return glm::vec3(0.0f, 1.0f, 0.0f);
        case TileFace::South:  return glm::vec3(0.0f, -1.0f, 0.0f);
        case TileFace::East:   return glm::vec3(1.0f, 0.0f, 0.0f);
        case TileFace::West:   return glm::vec3(-1.0f, 0.0f, 0.0f);
    }
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

/**
 * @brief The tile-grid step taken by moving one tile across `face`, for a lateral face.
 *
 * `{0, 0}` for Top/Bottom, which have no neighbour in the tile plane.
 *
 * @param face The face.
 * @return The (dx, dy) offset of the neighbouring column.
 */
inline glm::ivec2 face_step(TileFace face) {
    switch (face) {
        case TileFace::North: return glm::ivec2(0, 1);
        case TileFace::South: return glm::ivec2(0, -1);
        case TileFace::East:  return glm::ivec2(1, 0);
        case TileFace::West:  return glm::ivec2(-1, 0);
        default:              return glm::ivec2(0, 0);
    }
}

} // namespace world
} // namespace toy

namespace std {

/** @brief Hash for toy::world::ChunkCoord, so a chunk map can be an unordered_map. */
template <>
struct hash<toy::world::ChunkCoord> {
    std::size_t operator()(const toy::world::ChunkCoord& c) const noexcept {
        // Two 32-bit halves packed into one 64-bit key, then mixed: chunk coordinates are small
        // and highly correlated (a neighbourhood is a contiguous square), so a bare XOR would
        // collide every diagonal onto one bucket.
        const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32) |
                                   static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y));
        std::uint64_t h = key * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

} // namespace std

#endif // TOYENGINE_WORLD_TILE_TYPES_H
