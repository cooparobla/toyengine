/**
 * @file nav_debug.h
 * @brief Debug geometry for navigation, as plain line segments in the same debug::DebugDraw
 *        PhysicsWorld::debug_draw() fills: walkable-area outlines, the cell lattice, tile
 *        borders, off-mesh links, paths and flow-field arrows.
 */

#ifndef PHYSXCOOPA_NAV_NAV_DEBUG_H
#define PHYSXCOOPA_NAV_NAV_DEBUG_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_mesh.h>
#include <physxcoopa/nav/flow_field.h>
#include <physxcoopa/debug/debug_draw.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace coopa {
namespace physx {
namespace nav {

/** @brief Which navigation debug geometry to emit. */
enum class NavDebugFlags : uint32_t {
    None = 0,
    Mesh = 1u << 0,    /**< Outline of the walkable area (every edge without a link). */
    Grid = 1u << 1,    /**< Every cell's outline -- dense; for small scenes. */
    Tiles = 1u << 2,   /**< Tile borders at ground level. */
    Links = 1u << 3,   /**< Off-mesh links. */
    Paths = 1u << 4,   /**< Agents' current paths. */
    Flow = 1u << 5,    /**< Flow-field arrows (one per `flow_stride` cells). */
    Agents = 1u << 6,  /**< Agent circles and velocities. */
    FlowTiles = 1u << 7, /**< Hierarchical flow fields: exact tiles outlined, coarse arrows elsewhere. */
    All = Mesh | Tiles | Links | Paths | Flow | Agents | FlowTiles,
};

inline NavDebugFlags operator|(NavDebugFlags a, NavDebugFlags b) {
    return static_cast<NavDebugFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_flag(NavDebugFlags flags, NavDebugFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

inline NavDebugFlags parse_nav_debug_flag(const std::string& name) {
    if (name == "Mesh") return NavDebugFlags::Mesh;
    if (name == "Grid") return NavDebugFlags::Grid;
    if (name == "Tiles") return NavDebugFlags::Tiles;
    if (name == "Links") return NavDebugFlags::Links;
    if (name == "Paths") return NavDebugFlags::Paths;
    if (name == "Flow") return NavDebugFlags::Flow;
    if (name == "Agents") return NavDebugFlags::Agents;
    if (name == "FlowTiles") return NavDebugFlags::FlowTiles;
    if (name == "All") return NavDebugFlags::All;
    return NavDebugFlags::None;
}

namespace colors {
inline constexpr uint32_t k_nav_edge = 0x20E0FFFFu;
inline constexpr uint32_t k_nav_grid = 0x1F6F80FFu;
inline constexpr uint32_t k_nav_tile = 0x305070FFu;
inline constexpr uint32_t k_nav_link = 0xFFA020FFu;
inline constexpr uint32_t k_nav_path = 0xFFFF40FFu;
inline constexpr uint32_t k_nav_flow = 0x60FF60FFu;
inline constexpr uint32_t k_nav_agent = 0xFF60C0FFu;

/** @brief A stable colour per area id, so custom areas read apart from plain walkable. */
inline uint32_t area_color(uint8_t area) {
    if (area == k_area_walkable) return k_nav_edge;
    uint32_t h = static_cast<uint32_t>(area) * 2654435761u;
    uint32_t r = 96 + ((h >> 8) & 0x9F), g = 96 + ((h >> 16) & 0x9F), b = 96 + ((h >> 24) & 0x9F);
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}
} // namespace colors

/** @brief Emits mesh geometry (Mesh / Grid / Tiles / Links) for `mesh`. */
inline void debug_draw_mesh(const NavMesh& mesh, debug::DebugDraw& out, NavDebugFlags flags, float z_offset = 0.03f) {
    const NavGridParams& p = mesh.params;
    const float cs = p.cs;
    const bool edges = has_flag(flags, NavDebugFlags::Mesh);
    const bool grid = has_flag(flags, NavDebugFlags::Grid);
    if (edges || grid) {
        for (const auto& tile_ptr : mesh.tiles) {
            if (!tile_ptr) continue;
            const NavTile& t = *tile_ptr;
            uint32_t ti = mesh.tile_index(t.tx, t.ty);
            for (uint32_t s = 0; s < t.spans.size(); ++s) {
                const NavSpan& sp = t.spans[s];
                glm::ivec2 c = mesh.cell_of(make_ref(ti, s));
                float x0 = p.origin.x + static_cast<float>(c.x) * cs, y0 = p.origin.y + static_cast<float>(c.y) * cs;
                float z = p.floor_z(sp.floor) + z_offset;
                const glm::vec3 corner[4] = {{x0, y0, z}, {x0, y0 + cs, z}, {x0 + cs, y0 + cs, z}, {x0 + cs, y0, z}};
                // Edge d of the cell: d=0 -X (c0-c1), 1 +Y (c1-c2), 2 +X (c2-c3), 3 -Y (c3-c0).
                for (int d = 0; d < 4; ++d) {
                    bool open = sp.link[d] != k_no_link;
                    if (open && !grid) continue;
                    if (open && (d == 0 || d == 3)) continue; // grid: draw each shared edge once
                    out.add_line(corner[d], corner[(d + 1) & 3], open ? colors::k_nav_grid : colors::area_color(sp.area));
                }
            }
        }
    }
    if (has_flag(flags, NavDebugFlags::Tiles)) {
        for (const auto& tile_ptr : mesh.tiles) {
            if (!tile_ptr) continue;
            geometry::AABB b = p.tile_bounds(tile_ptr->tx, tile_ptr->ty);
            float z = p.origin.z;
            float zmax = z;
            for (const auto& sp : tile_ptr->spans) zmax = std::max(zmax, p.floor_z(sp.floor));
            z = zmax + z_offset;
            out.add_line({b.min.x, b.min.y, z}, {b.max.x, b.min.y, z}, colors::k_nav_tile);
            out.add_line({b.min.x, b.min.y, z}, {b.min.x, b.max.y, z}, colors::k_nav_tile);
        }
    }
    if (has_flag(flags, NavDebugFlags::Links)) {
        for (const OffMeshLink& l : mesh.links) {
            bool resolved = l.start_ref != k_invalid_span && l.end_ref != k_invalid_span;
            uint32_t col = resolved ? colors::k_nav_link : 0xFF2020FFu;
            glm::vec3 prev = l.start;
            float lift = std::max(0.3f, 0.25f * glm::distance(l.start, l.end));
            for (int i = 1; i <= 12; ++i) {
                float t = static_cast<float>(i) / 12.0f;
                glm::vec3 q = glm::mix(l.start, l.end, t) + glm::vec3(0.0f, 0.0f, 4.0f * t * (1.0f - t) * lift);
                out.add_line(prev, q, col);
                prev = q;
            }
            out.add_line(l.start, l.start + glm::vec3(0, 0, 0.3f), col);
            out.add_line(l.end, l.end + glm::vec3(0, 0, 0.3f), col);
        }
    }
}

/** @brief A path as a polyline with a small post at each corner. */
inline void debug_draw_path(const NavPath& path, debug::DebugDraw& out, uint32_t color = colors::k_nav_path,
                            float z_offset = 0.08f) {
    glm::vec3 up(0.0f, 0.0f, z_offset);
    for (std::size_t i = 0; i < path.points.size(); ++i) {
        if (i > 0) out.add_line(path.points[i - 1] + up, path.points[i] + up, color);
        out.add_line(path.points[i], path.points[i] + up * 3.0f, color);
    }
}

namespace colors {
inline constexpr uint32_t k_nav_flow_tile = 0x40FF90FFu;
inline constexpr uint32_t k_nav_flow_coarse = 0xC0A040FFu;
} // namespace colors

/**
 * @brief For a hierarchical field: the outline of every exactly integrated tile, and one arrow
 *        per routed region elsewhere (centroid toward its best portal) -- the coarse layer.
 */
inline void debug_draw_flow_tiles(const FlowField& flow, debug::DebugDraw& out, float z_offset = 0.1f,
                                  uint32_t tile_color = colors::k_nav_flow_tile, uint32_t coarse_color = colors::k_nav_flow_coarse) {
    if (!flow.hierarchical()) return;
    const NavMesh& mesh = flow.mesh();
    const NavGridParams& p = mesh.params;
    for (const auto& tile_ptr : mesh.tiles) {
        if (!tile_ptr) continue;
        const NavTile& t = *tile_ptr;
        uint32_t ti = mesh.tile_index(t.tx, t.ty);
        if (flow.tile_active(ti)) {
            geometry::AABB b = p.tile_bounds(t.tx, t.ty);
            float z = t.regions.empty() ? p.origin.z : t.regions[0].centroid.z;
            for (const auto& r : t.regions) z = std::max(z, r.centroid.z);
            z += z_offset;
            const float in = p.cs * 0.5f;
            glm::vec3 c[4] = {{b.min.x + in, b.min.y + in, z}, {b.max.x - in, b.min.y + in, z},
                              {b.max.x - in, b.max.y - in, z}, {b.min.x + in, b.max.y - in, z}};
            for (int i = 0; i < 4; ++i) out.add_line(c[i], c[(i + 1) & 3], tile_color);
            continue;
        }
        for (uint16_t r = 0; r < t.regions.size(); ++r) {
            const NavRegion& reg = t.regions[r];
            if (reg.span_count < 8) continue;
            glm::vec3 exit;
            if (!(flow.coarse_estimate_region(make_ref(ti, r), reg.centroid, &exit) < std::numeric_limits<float>::infinity())) continue;
            glm::vec3 a = reg.centroid + glm::vec3(0.0f, 0.0f, z_offset);
            glm::vec2 v = glm::vec2(exit) - glm::vec2(a);
            float l = glm::length(v);
            if (l < 1e-3f) continue;
            float len = std::min(l, static_cast<float>(p.tile_size) * p.cs * 0.4f);
            glm::vec2 dir = v / l;
            glm::vec3 b = a + glm::vec3(dir * len, 0.0f);
            out.add_line(a, b, coarse_color);
            glm::vec2 side(-dir.y, dir.x);
            out.add_line(b, b - glm::vec3(dir * (len * 0.25f) + side * (len * 0.15f), 0.0f), coarse_color);
            out.add_line(b, b - glm::vec3(dir * (len * 0.25f) - side * (len * 0.15f), 0.0f), coarse_color);
        }
    }
}

/** @brief One arrow per `stride` x `stride` cells of every reached span. */
inline void debug_draw_flow(const FlowField& flow, debug::DebugDraw& out, int stride = 2, uint32_t color = colors::k_nav_flow,
                            float z_offset = 0.06f) {
    const NavMesh& mesh = flow.mesh();
    const float len = mesh.params.cs * static_cast<float>(std::max(stride, 1)) * 0.7f;
    stride = std::max(stride, 1);
    for (const auto& tile_ptr : mesh.tiles) {
        if (!tile_ptr) continue;
        const NavTile& t = *tile_ptr;
        uint32_t ti = mesh.tile_index(t.tx, t.ty);
        for (uint32_t s = 0; s < t.spans.size(); ++s) {
            SpanRef r = make_ref(ti, s);
            glm::ivec2 c = mesh.cell_of(r);
            if ((c.x % stride) != 0 || (c.y % stride) != 0) continue;
            float d;
            glm::vec2 dir;
            if (!flow.span_value(r, d, dir)) continue;
            glm::vec3 a = mesh.position_of(r) + glm::vec3(0.0f, 0.0f, z_offset);
            glm::vec3 b = a + glm::vec3(dir * len, 0.0f);
            out.add_line(a, b, color);
            glm::vec2 side(-dir.y, dir.x);
            out.add_line(b, b - glm::vec3(dir * (len * 0.35f) + side * (len * 0.2f), 0.0f), color);
            out.add_line(b, b - glm::vec3(dir * (len * 0.35f) - side * (len * 0.2f), 0.0f), color);
        }
    }
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_DEBUG_H
