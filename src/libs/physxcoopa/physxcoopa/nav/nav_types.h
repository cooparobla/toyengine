/**
 * @file nav_types.h
 * @brief Shared vocabulary for the navigation module: span references, build settings, agent
 *        profiles, area costs and query filters.
 *
 * The navigation "mesh" is a tiled, multi-layer voxel surface (a compact heightfield in Recast's
 * terms) rather than a polygon mesh: every walkable cell column holds one span per floor it
 * contains, and spans link to the spans they can step to in the four neighbouring columns. That
 * one structure serves both search styles this module offers -- A* (path_query.h) walks it as a
 * graph, and flow fields (flow_field.h) integrate over it as a grid -- and it is 3D throughout:
 * a stair is a chain of spans whose floors rise by less than the agent's climb, so going
 * upstairs is ordinary graph search.
 */

#ifndef PHYSXCOOPA_NAV_NAV_TYPES_H
#define PHYSXCOOPA_NAV_NAV_TYPES_H

#include <physxcoopa/geometry/aabb.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @brief Reference to one span: `(tile_index << 16) | span_index`.
 *
 * Valid only against the NavMesh it came from. A tile rebuild renumbers that tile's spans, so
 * long-lived state (agent positions, paths) is kept as world positions and re-resolved with
 * NavMesh::find_nearest(), never as a cached SpanRef.
 */
using SpanRef = uint32_t;
inline constexpr SpanRef k_invalid_span = 0xFFFFFFFFu;

inline constexpr uint32_t ref_tile(SpanRef r) { return r >> 16; }
inline constexpr uint32_t ref_span(SpanRef r) { return r & 0xFFFFu; }
inline constexpr SpanRef make_ref(uint32_t tile, uint32_t span) { return (tile << 16) | (span & 0xFFFFu); }

/** @brief Reference to one region (a tile-local connected patch of spans) -- the node type of
 *         the coarse graph. Same packing as SpanRef: `(tile_index << 16) | region_index`. */
using RegionRef = uint32_t;
inline constexpr RegionRef k_invalid_region = 0xFFFFFFFFu;

/** @brief "No link in this direction" marker in NavSpan::link. */
inline constexpr uint8_t k_no_link = 0xFF;
/** @brief Open span with no ceiling above it. */
inline constexpr uint16_t k_open_ceiling = 0xFFFF;

/** @brief Direction order used by every per-direction array: -X, +Y, +X, -Y (Recast's order). */
inline constexpr int k_dir_dx[4] = {-1, 0, 1, 0};
inline constexpr int k_dir_dy[4] = {0, 1, 0, -1};
inline constexpr int opposite_dir(int d) { return (d + 2) & 3; }

// --- Areas ---------------------------------------------------------------------------------

/** @brief Area id 0 is "not walkable": spans are never stored with it. */
inline constexpr uint8_t k_area_null = 0;
/** @brief The area every walkable surface gets unless a NavModifier/NavVolume says otherwise. */
inline constexpr uint8_t k_area_walkable = 1;
/** @brief Number of area ids (0..63). */
inline constexpr uint32_t k_max_areas = 64;

/**
 * @struct AreaTable
 * @brief Named area types with a default traversal cost. Index == area id.
 *
 * Costs multiply distance: a path through an area of cost 3 is three times as "long" as the
 * same distance through cost 1. Filters may override costs per query (QueryFilter::area_cost).
 */
struct AreaTable {
    std::array<std::string, k_max_areas> names{};
    std::array<float, k_max_areas> costs{};

    AreaTable() {
        costs.fill(1.0f);
        names[k_area_null] = "NotWalkable";
        names[k_area_walkable] = "Walkable";
    }

    /** @brief The id named `name`, or `fallback` if none. Accepts a bare number too. */
    uint8_t find(const std::string& name, uint8_t fallback = k_area_walkable) const {
        for (uint32_t i = 0; i < k_max_areas; ++i) {
            if (!names[i].empty() && names[i] == name) return static_cast<uint8_t>(i);
        }
        char* end = nullptr;
        long v = std::strtol(name.c_str(), &end, 10);
        if (end && *end == '\0' && !name.empty() && v >= 0 && v < static_cast<long>(k_max_areas)) {
            return static_cast<uint8_t>(v);
        }
        return fallback;
    }
};

// --- Agents ---------------------------------------------------------------------------------

/**
 * @struct AgentProfile
 * @brief The body a navmesh is built for (Unity's "agent type"). Each profile gets its own
 *        NavMesh: radius erodes walkable area away from walls, height rejects low ceilings,
 *        and max_climb decides which height steps link (stairs, curbs).
 */
struct AgentProfile {
    std::string name = "Humanoid";
    float radius = 0.4f;
    float height = 1.8f;
    float max_climb = 0.45f;
};

// --- Build settings -------------------------------------------------------------------------

/**
 * @struct NavBuildSettings
 * @brief Voxelization and tiling parameters shared by every agent profile.
 */
struct NavBuildSettings {
    /** @brief XY size of one cell, metres. Paths and flow vectors have this resolution. */
    float cell_size = 0.25f;
    /** @brief Z quantization of span floors and ceilings, metres. */
    float cell_height = 0.1f;
    /** @brief Cells per tile side. Tiles are the unit of parallel and incremental building. */
    int tile_size = 32;
    /** @brief Steepest walkable surface, degrees from horizontal. */
    float max_slope_degrees = 45.0f;

    /** @brief World bounds to build. An empty box (the default) means "fit the sources". */
    geometry::AABB bounds{};
    /** @brief Padding added around auto-fitted bounds, metres. */
    float bounds_padding = 2.0f;

    std::vector<AgentProfile> agents{AgentProfile{}};
    AreaTable areas{};

    /** @brief Physics layers whose colliders contribute geometry (bit per layer). */
    uint32_t layer_mask = ~0u;

    /** @brief Seconds a tile must stay dirty before an incremental rebuild starts, so a burst
     *         of edits (or an obstacle sliding to rest) rebuilds once, not every frame. */
    float rebuild_delay = 0.25f;
    /** @brief Cap on tiles rebuilt concurrently in the background. */
    int max_concurrent_rebuilds = 64;

    /** @brief Index of the profile named `name`, or 0. */
    uint32_t agent_index(const std::string& name) const {
        for (uint32_t i = 0; i < agents.size(); ++i) {
            if (agents[i].name == name) return i;
        }
        return 0;
    }
};

/**
 * @struct NavGridParams
 * @brief The fixed grid one NavMesh lives on, derived from NavBuildSettings + bounds.
 */
struct NavGridParams {
    glm::vec3 origin{0.0f};      /**< World position of cell (0,0) corner and z quantum 0. */
    float cs = 0.25f;            /**< Cell size (XY). */
    float ch = 0.1f;             /**< Cell height (Z). */
    int tile_size = 32;
    int tiles_x = 0;
    int tiles_y = 0;

    // Per-agent values, in cells:
    int walkable_height = 18;   /**< ceil(height / ch). */
    int walkable_climb = 4;     /**< floor(max_climb / ch). */
    int walkable_radius = 2;    /**< ceil(radius / cs). */
    int border = 3;             /**< Padding cells rasterized around a tile (radius + 1). */
    float agent_radius = 0.4f;
    float agent_height = 1.8f;

    int cells_x() const { return tiles_x * tile_size; }
    int cells_y() const { return tiles_y * tile_size; }
    uint32_t tile_count() const { return static_cast<uint32_t>(tiles_x * tiles_y); }

    /** @brief Global cell containing world (x, y), unclamped. */
    glm::ivec2 cell_of(const glm::vec3& p) const {
        return {static_cast<int>(std::floor((p.x - origin.x) / cs)),
                static_cast<int>(std::floor((p.y - origin.y) / cs))};
    }
    bool cell_in_bounds(int cx, int cy) const {
        return cx >= 0 && cy >= 0 && cx < cells_x() && cy < cells_y();
    }
    /** @brief World XY centre of a cell, at height `z`. */
    glm::vec3 cell_center(int cx, int cy, float z) const {
        return {origin.x + (static_cast<float>(cx) + 0.5f) * cs, origin.y + (static_cast<float>(cy) + 0.5f) * cs, z};
    }
    float floor_z(uint16_t q) const { return origin.z + static_cast<float>(q) * ch; }
    int quantize_z_floor(float z) const { return static_cast<int>(std::floor((z - origin.z) / ch)); }

    /** @brief World bounds of tile (tx, ty) (z spans everything). */
    geometry::AABB tile_bounds(int tx, int ty, int pad_cells = 0) const {
        geometry::AABB b;
        float ts = static_cast<float>(tile_size) * cs;
        float pad = static_cast<float>(pad_cells) * cs;
        b.min = {origin.x + static_cast<float>(tx) * ts - pad, origin.y + static_cast<float>(ty) * ts - pad, -std::numeric_limits<float>::max()};
        b.max = {origin.x + static_cast<float>(tx + 1) * ts + pad, origin.y + static_cast<float>(ty + 1) * ts + pad, std::numeric_limits<float>::max()};
        return b;
    }
};

// --- Queries --------------------------------------------------------------------------------

/**
 * @struct QueryFilter
 * @brief Per-query traversal rules layered on top of the agent profile the mesh was built for.
 */
struct QueryFilter {
    /** @brief Bit per area id; spans of an excluded area are never entered. */
    uint64_t area_mask = ~0ull;
    /** @brief Multiplier per area id. Initialised from the AreaTable by NavMesh::default_filter(). */
    std::array<float, k_max_areas> area_cost{};
    /** @brief Extra clearance from walls beyond the baked agent radius, metres (0 = none). */
    float extra_clearance = 0.0f;
    /** @brief Cost penalty for hugging walls: spans within `wall_penalty_distance` of a wall
     *         cost up to (1 + wall_penalty) times as much. 0 disables it. */
    float wall_penalty = 0.0f;
    float wall_penalty_distance = 1.0f;
    /** @brief A* heuristic weight; > 1 trades optimality for fewer expansions. The default
     *         1.1 expands about a third as many nodes as 1.0 in cluttered levels for paths
     *         ~3% longer before smoothing; set 1.0 for exactly optimal paths. */
    float heuristic_weight = 1.1f;
    /** @brief Maximum A* node expansions before giving up (with a partial result). */
    uint32_t max_nodes = 65536;
    /** @brief Return the path to the closest reachable point when the goal can't be reached. */
    bool allow_partial = true;

    QueryFilter() { area_cost.fill(1.0f); }

    bool passes_area(uint8_t area) const { return area != k_area_null && ((area_mask >> area) & 1ull); }
};

/** @brief Outcome of a path query. */
enum class PathStatus : uint8_t {
    Success,      /**< Complete path to the goal. */
    Partial,      /**< Goal unreachable (or budget hit): path to the closest reachable point. */
    NoPath,       /**< No path at all (start or goal off the mesh, or disconnected and !allow_partial). */
};

/** @brief Per-point flags on a NavPath. */
enum NavPathPointFlags : uint8_t {
    k_path_point_none = 0,
    k_path_point_link_start = 1u << 0, /**< Next segment is an off-mesh link (jump/ladder/drop). */
    k_path_point_link_end = 1u << 1,
};

/**
 * @struct NavPath
 * @brief A smoothed path: corner points on the walkable surface (z = floor height).
 */
struct NavPath {
    PathStatus status = PathStatus::NoPath;
    std::vector<glm::vec3> points;
    std::vector<uint8_t> flags;   /**< Parallel to points. */
    float length = 0.0f;
    uint32_t nodes_expanded = 0;  /**< Diagnostics. */

    bool valid() const { return status != PathStatus::NoPath && !points.empty(); }
};

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_TYPES_H
