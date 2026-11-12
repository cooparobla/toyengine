/**
 * @file tile_types.h
 * @brief The vocabulary of the terrain tile system: faces, surface kinds, columns and chunk
 *        coordinates, plus the D4 orientations tile-set pieces are baked into.
 *
 * A terrain **tile** is one cell of the world grid, extruded from z = 0 up to its own height.
 * Its visible geometry is assembled from a **tile set**'s authored pieces (tile_topology.h,
 * tile_mesh_library.h): a top always, and walls only against a lower neighbour. The chunk
 * mesher (terrain_chunk.h) merges every placed piece in a chunk into one GPU mesh.
 *
 * Z is up in this engine (see assets/config.yaml's `gravity: {0, 0, -9.81}`), so mapcoopa's
 * grid (x, y) maps straight onto world (x, y) and elevation onto world z.
 */

#ifndef TOYENGINE_WORLD_TILE_TYPES_H
#define TOYENGINE_WORLD_TILE_TYPES_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <toyengine/world/terrain_types.h>

namespace toy {
namespace world {

/**
 * @enum TileFace
 * @brief The six sides of a tile, named by the axis each one faces.
 *
 * `Top` is first: it is the only side a flat-ground tile ever shows.
 */
enum class TileFace : std::uint8_t {
    Top,    /**< @brief +Z, the canonical authoring orientation. */
    Bottom, /**< @brief -Z; never exposed (a column rests on the ground). */
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
 * Appended, never reordered: a kind's position IS its atlas cell (row-major in a
 * k_atlas_columns x k_atlas_rows grid; terrain_styled.frag reads each cell's centre), so
 * inserting above would silently re-colour the whole world.
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
    Salt,
    /** @brief Packed, trodden roadway -- the surface a street or a highway is laid in. */
    Road
};

/** @brief Number of distinct `TileKind` values; the number of occupied atlas cells. */
inline constexpr std::size_t k_tile_kind_count = 13;

/** @brief Atlas cells per row. 4 x 4 leaves four spare cells for new kinds without a re-layout. */
inline constexpr std::uint32_t k_atlas_columns = 4;
/** @brief Atlas cells per column. */
inline constexpr std::uint32_t k_atlas_rows = 4;
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
    /** @brief The terrace the water over this column fills to, in steps; 0 when dry. */
    std::int32_t water_level = 0;
    /** @brief What the water is: 0 dry, 1 still (sea or lake), 2 flowing (river). */
    std::uint8_t water = 0;
    /** @brief Flowing water's downstream direction: an octant 1..8 (k_flow_directions), 0 none. */
    std::uint8_t flow = 0;
};

/** @brief TileColumn::water values. */
inline constexpr std::uint8_t k_water_none = 0;
inline constexpr std::uint8_t k_water_still = 1;
inline constexpr std::uint8_t k_water_flowing = 2;

/** @brief The eight downstream directions TileColumn::flow / TileRecord::flow name, 1-based. */
inline constexpr std::array<std::array<int, 2>, 8> k_flow_directions = {{
    {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};

/** @brief The direction a flow octant (1..8) names; zero for 0. */
inline glm::vec2 flow_direction(std::uint8_t octant) {
    if (octant < 1 || octant > 8) return glm::vec2(0.0f);
    const auto& d = k_flow_directions[octant - 1];
    return glm::normalize(glm::vec2(static_cast<float>(d[0]), static_cast<float>(d[1])));
}

/** @brief The flow octant (1..8) nearest a direction; 0 for a zero vector. */
inline std::uint8_t flow_octant(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) return 0;
    const double turns = std::atan2(dy, dx) / (2.0 * 3.14159265358979323846);
    const int octant = static_cast<int>(std::lround(turns * 8.0)) & 7;
    return static_cast<std::uint8_t>(octant + 1);
}

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
 * @brief The unit outward normal of `face`, in tile space.
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

/** @brief Number of D4 orientations a styled tile piece is pre-baked in; see variant_transform(). */
inline constexpr std::size_t k_tile_orientation_count = 8;

/**
 * @brief The D4 element `orientation` names, as a transform of the unit cell about its vertical axis.
 *
 * Used by styled tiles (tile_topology.h): every piece is authored once, in one canonical corner
 * of the cell, and placed by one of these. Bit 2 mirrors across the `x = 0.5` plane; bits 0-1
 * then turn the result that many quarter turns counter-clockwise about +Z through the cell
 * centre. **Mirror first, then rotate.** Both act about `(0.5, 0.5)`, so the result still spans
 * `[0,1]^3` and is placed by a plain translate, exactly like face_transform().
 *
 * @param orientation `0..7`.
 * @return The object-space transform.
 */
inline const glm::mat4& variant_transform(std::uint8_t orientation) {
    static const std::array<glm::mat4, k_tile_orientation_count> table = [] {
        const glm::vec3 centre(0.5f, 0.5f, 0.0f);
        std::array<glm::mat4, k_tile_orientation_count> out{};
        for (std::size_t o = 0; o < k_tile_orientation_count; ++o) {
            glm::mat4 mirror(1.0f);
            if (o & 4u) mirror[0][0] = -1.0f;
            const glm::mat4 rotate =
                glm::rotate(glm::mat4(1.0f), glm::radians(90.0f * static_cast<float>(o & 3u)),
                            glm::vec3(0.0f, 0.0f, 1.0f));
            out[o] = glm::translate(glm::mat4(1.0f), centre) * rotate * mirror *
                     glm::translate(glm::mat4(1.0f), -centre);
        }
        return out;
    }();
    return table[orientation & 7u];
}

/** @brief True when `orientation` includes the mirror -- a baked piece must reverse its winding. */
inline bool orientation_mirrors(std::uint8_t orientation) {
    return (orientation & 4u) != 0;
}

/**
 * @brief What sits immediately under a given top surface, on a lateral face.
 *
 * The grass-over-dirt read every block game uses: a cut through turf shows soil, a cut through
 * sand shows sand. Below `TerrainParams::soil_depth_steps` the mesher overrides this with bare
 * stone regardless -- see mesh_chunk_styled().
 *
 * @param top The column's top surface.
 * @return The surface its upper lateral faces are textured with.
 */
inline TileKind side_kind_for(TileKind top) {
    switch (top) {
        case TileKind::Grass:
        case TileKind::Moss:  return TileKind::Dirt;
        case TileKind::Snow:  return TileKind::Stone;
        case TileKind::Sand:  return TileKind::Sand;
        case TileKind::Clay:  return TileKind::Clay;
        case TileKind::Salt:  return TileKind::Clay;
        case TileKind::Road:  return TileKind::Dirt;
        case TileKind::Ash:   return TileKind::Stone;
        case TileKind::Ice:   return TileKind::Ice;
        case TileKind::Water: return TileKind::Water;
        default:              return TileKind::Stone;
    }
}

/** @brief Every kind's lower-case name, in TileKind order -- also the built-in layer names. */
inline const char* const* tile_kind_names() {
    static const char* const names[k_tile_kind_count] = {
        "grass", "dirt", "stone", "sand", "snow", "rock", "water", "ice", "moss", "clay", "ash",
        "salt",  "road"};
    return names;
}

/** @brief The lower-case name of one kind, as scene YAML spells it. */
inline const char* tile_kind_name(TileKind kind) {
    const std::size_t i = static_cast<std::size_t>(kind);
    return i < k_tile_kind_count ? tile_kind_names()[i] : "grass";
}

/**
 * @brief Resolves a lower-case kind name (`"grass"`, `"stone"`, ...) to its TileKind.
 *
 * @param name  The name, as scene YAML spells it.
 * @param out   Receives the kind when the name is known.
 * @return False for an unknown name; `out` is left untouched.
 */
inline bool tile_kind_from_name(const char* name, TileKind& out) {
    const char* const* names = tile_kind_names();
    for (std::size_t i = 0; i < k_tile_kind_count; ++i) {
        const char* a = names[i];
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) {
            out = static_cast<TileKind>(i);
            return true;
        }
    }
    return false;
}

/**
 * @brief The layer list a tiles-mode terrain wears when its scene authors none: one layer per
 *        TileKind, in TileKind order, each taking its colour and roughness from its atlas cell
 *        and drawing its detail procedurally.
 *
 * A terrain's surface nibble is an index into its layer list (TerrainLayer), so these entries are
 * what make an unlisted tile world look the way it always has: index 0 is grass, 2 is stone, and
 * so on. A scene that writes its own `layers:` replaces the list wholesale.
 */
inline std::vector<TerrainLayer> builtin_tile_layers() {
    std::vector<TerrainLayer> out;
    out.reserve(k_tile_kind_count);
    for (std::size_t i = 0; i < k_tile_kind_count; ++i) {
        TerrainLayer l;
        l.name = tile_kind_names()[i];
        l.atlas_cell = static_cast<int>(i);
        l.procedural = true;
        l.tiling = 1.0f;   // Metres per repeat of the procedural detail, which is world-space.
        out.push_back(std::move(l));
    }
    return out;
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
