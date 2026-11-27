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
 * A *surface*, not a biome: thirty-seven biomes (see coopa::maps::Biome) collapse onto this
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
    Road,
    /**
     * @brief Molten rock: the only kind that emits light rather than only reflecting it.
     *
     * The emission is not in the albedo -- it comes from the metallic-roughness atlas's red
     * channel, which nothing else reads (see `gen_terrain_atlas.py` and
     * `assets/shaders/terrain_styled.glsl`). A cooled crust with incandescent cracks reads as
     * lava; a uniformly orange surface reads as orange rock.
     */
    Lava,
    /** @brief Cooled lava: dark, fine-grained, the ground a flow leaves behind. */
    Basalt
};

/** @brief Number of distinct `TileKind` values; the number of occupied atlas cells. */
inline constexpr std::size_t k_tile_kind_count = 15;

/** @brief Atlas cells per row. 4 x 4 leaves one spare cell for a new kind without a re-layout. */
inline constexpr std::uint32_t k_atlas_columns = 4;
/** @brief Atlas cells per column. */
inline constexpr std::uint32_t k_atlas_rows = 4;
/** @brief Sub-step quanta in one whole tile step -- the resolution of `TileColumn::sub`. */
inline constexpr std::int32_t k_sub_steps = 256;

/**
 * @brief Clamps a `TerrainParams::height_fine` key to the cuts per step a bake will really use.
 *
 * Any count in `[1, k_sub_steps]`; 1 means whole steps. Five is as good as four -- the storage
 * stays 1/`k_sub_steps` of a step whatever the count, so a cut lands on the NEAREST stored
 * quantum rather than an exact one, and five cuts of a one-metre step come out 2 mm off a true
 * 20 cm. What has to be exact is that a given cut always produces the SAME stored value, which
 * fine_for_cut() guarantees -- that is what lets two neighbours on one cut share a height
 * exactly, and so need no wall between them.
 */
inline std::int32_t sub_steps_for(std::int32_t height_fine) {
    return height_fine <= 1 ? 1 : (height_fine > k_sub_steps ? k_sub_steps : height_fine);
}

/** @brief The stored height, in quanta, of cut number `index` on a grid of `cuts` per step. */
inline std::int32_t fine_for_cut(std::int64_t index, std::int32_t cuts) {
    if (cuts <= 1) return static_cast<std::int32_t>(index) * k_sub_steps;
    const std::int64_t num = index * k_sub_steps;
    // Round to nearest, flooring toward negative infinity so a cut below the datum behaves.
    const std::int64_t q = num >= 0 ? (num + cuts / 2) / cuts
                                    : -((-num + cuts / 2) / cuts);
    return static_cast<std::int32_t>(q);
}

/** @brief Quantises a height in steps onto a grid of `cuts` per step, as quanta. */
inline std::int32_t quantise_fine(double value_in_steps, std::int32_t cuts) {
    if (cuts <= 1) return static_cast<std::int32_t>(std::floor(value_in_steps)) * k_sub_steps;
    return fine_for_cut(static_cast<std::int64_t>(std::floor(value_in_steps * cuts)), cuts);
}

/**
 * @struct TileColumn
 * @brief One tile's sampled state: how tall it is and what it is made of.
 *
 * `steps` is a COUNT of vertical tile steps, not a height in world units -- the quantisation is
 * what gives the terraced, block-stacked read, and keeping it integral is what lets the mesher
 * decide face exposure with an exact comparison rather than an epsilon one.
 *
 * `sub` refines where the top of that stack of steps actually sits without giving up either
 * property: it is a COUNT too, of `1/k_sub_steps` of a step, so `fine()` compares two columns
 * exactly. A terraced world leaves it 0 and every comparison below reduces to the whole-step
 * one it has always been.
 */
struct TileColumn {
    std::int32_t steps     = 0;                 /**< @brief Height in whole tile steps above z = 0. */
    /**
     * @brief How far above `steps` this column's top sits, in `1/k_sub_steps` of a step, 0..255.
     *
     * The column is still built out of whole steps -- this raises the whole stack, so the top
     * face stays flat and every stamped piece keeps its exact height. Only `fine()` and the
     * walls that close the difference read it.
     */
    std::int32_t sub       = 0;
    TileKind     top_kind  = TileKind::Grass;   /**< @brief Surface of the top face. */
    TileKind     side_kind = TileKind::Dirt;    /**< @brief Surface of the lateral faces just under the top. */
    /** @brief The terrace the water over this column fills to, in steps; 0 when dry. */
    std::int32_t water_level = 0;
    /** @brief What the water is: 0 dry, 1 still (sea or lake), 2 flowing (river). */
    std::uint8_t water = 0;
    /** @brief Flowing water's downstream direction: an octant 1..8 (k_flow_directions), 0 none. */
    std::uint8_t flow = 0;

    /**
     * @brief The underside of the solid roof over this column, in steps; see `roofed`.
     *
     * The column is solid 0..`steps`, open above it, and solid again `roof_steps`..`roof_top`.
     * A mesher that knows nothing about roofs reads `steps` exactly as it always did.
     */
    std::int32_t roof_steps = 0;
    /** @brief The top of that roof: its walkable surface, in steps. */
    std::int32_t roof_top = 0;
    TileKind     roof_top_kind  = TileKind::Stone; /**< @brief Surface of the roof's upper face. */
    TileKind     roof_side_kind = TileKind::Stone; /**< @brief Surface of the roof's lateral faces. */
    /** @brief Whether this column has a roof at all. False on every world that has none. */
    bool         roofed = false;
    /**
     * @brief Where this column's roofs are in `ColumnPad::roofs`, when it has more than one.
     *
     * `count` is 0 for a column with no roof or exactly one -- which is every column of every
     * world with no caves in it, and most columns of one that has them. The single roof lives in
     * the fields above and costs no arena entry; only a stacked column needs the list, and the
     * fields above still describe its OUTERMOST roof so anything reading them is never wrong,
     * only incomplete.
     */
    std::int32_t roof_first = 0;
    std::int32_t roof_count = 0;

    /**
     * @brief The outermost solid surface of this column: the roof's top where there is one, the
     *        ground otherwise.
     *
     * What anything looking at the world FROM OUTSIDE should read. A coarse tier that read
     * `steps` on a roofed column would draw the floor and leave the roof -- the only part
     * visible from out there -- missing entirely.
     */
    std::int32_t outer_top() const { return roofed ? roof_top : steps; }
    /**
     * @brief This column's top in sub-step quanta: the exact value to compare two columns by.
     *
     * `steps` alone answers "which terrace", which is what decides a piece's SHAPE (its lip, its
     * corners, whether a top quadrant is flat). `fine()` answers "how high", which is what
     * decides whether a wall has to close a drop at all.
     */
    std::int32_t fine() const { return steps * k_sub_steps + sub; }
    /** @brief `fine()` for the outermost surface, matching `outer_top()`. Roofs stay on steps. */
    std::int32_t outer_fine() const { return roofed ? roof_top * k_sub_steps : fine(); }
    /** @brief The outermost surface's kind, to match `outer_top()`. */
    TileKind outer_top_kind() const { return roofed ? roof_top_kind : top_kind; }
};

/** @brief TileColumn::water values. */
inline constexpr std::uint8_t k_water_none = 0;
inline constexpr std::uint8_t k_water_still = 1;
inline constexpr std::uint8_t k_water_flowing = 2;
/**
 * @brief Molten rock. Meshes as a still surface, but kept distinct from `k_water_still`.
 *
 * The distinction is what makes a lava world still lava after it has been RELOADED. What a
 * basin is filled with is stored per column (`TileCell::water_kind`), so it survives a bake, a
 * `.terra` on disk and a scene reopening -- but only if the column's kind reaches the body that
 * is built from it. Folded into `k_water_still` here, it does not, and the body comes back as
 * water the next time the world is loaded rather than generated.
 */
inline constexpr std::uint8_t k_water_lava = 3;

/** @brief Whether a column's fluid meshes as a still surface rather than a flowing one. */
inline constexpr bool column_water_is_still(std::uint8_t water) {
    return water == k_water_still || water == k_water_lava;
}

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
        // A lava surface is a skin over the rock it flowed from, so the wall under it is the
        // rock -- a cliff of glowing lava would read as a wall of fire.
        case TileKind::Lava:   return TileKind::Basalt;
        case TileKind::Basalt: return TileKind::Basalt;
        case TileKind::Ice:   return TileKind::Ice;
        case TileKind::Water: return TileKind::Water;
        default:              return TileKind::Stone;
    }
}

/** @brief Every kind's lower-case name, in TileKind order -- also the built-in layer names. */
inline const char* const* tile_kind_names() {
    static const char* const names[k_tile_kind_count] = {
        "grass", "dirt", "stone", "sand", "snow", "rock", "water", "ice", "moss", "clay", "ash",
        "salt",  "road", "lava",  "basalt"};
    return names;
}

/**
 * @brief Every kind's base colour, 0..1, in TileKind order.
 *
 * The same figures `tools/gen_terrain_atlas.py` bakes into `textures/terrain/terrain_atlas.png`,
 * which is what a chunk's shader actually samples -- these are for code that has to agree with a
 * tile's colour WITHOUT sampling the atlas: a map, a minimap, a low-detail stand-in drawn before
 * the chunks exist. **If one table moves, the other has to move with it**; the generator's
 * `KINDS` list is the authority and carries the same note.
 *
 * The shader varies each by +/-50% procedurally (`terrain_styled.glsl`), so a colour here is the
 * surface's mean rather than any particular texel of it.
 */
inline const glm::vec3* tile_kind_base_colors() {
    static const glm::vec3 colors[k_tile_kind_count] = {
        {118.0f / 255.0f, 184.0f / 255.0f,  76.0f / 255.0f},   // grass
        {158.0f / 255.0f, 110.0f / 255.0f,  72.0f / 255.0f},   // dirt
        {138.0f / 255.0f, 140.0f / 255.0f, 150.0f / 255.0f},   // stone
        {236.0f / 255.0f, 222.0f / 255.0f, 170.0f / 255.0f},   // sand
        {244.0f / 255.0f, 247.0f / 255.0f, 252.0f / 255.0f},   // snow
        {112.0f / 255.0f, 108.0f / 255.0f, 116.0f / 255.0f},   // rock
        { 78.0f / 255.0f, 150.0f / 255.0f, 205.0f / 255.0f},   // water
        {182.0f / 255.0f, 224.0f / 255.0f, 240.0f / 255.0f},   // ice
        {112.0f / 255.0f, 160.0f / 255.0f,  84.0f / 255.0f},   // moss
        {190.0f / 255.0f, 138.0f / 255.0f,  96.0f / 255.0f},   // clay
        { 96.0f / 255.0f,  90.0f / 255.0f,  88.0f / 255.0f},   // ash
        {238.0f / 255.0f, 236.0f / 255.0f, 226.0f / 255.0f},   // salt
        {150.0f / 255.0f, 128.0f / 255.0f, 100.0f / 255.0f},   // road
        {196.0f / 255.0f,  58.0f / 255.0f,  22.0f / 255.0f},   // lava
        { 54.0f / 255.0f,  50.0f / 255.0f,  56.0f / 255.0f}};  // basalt
    return colors;
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
