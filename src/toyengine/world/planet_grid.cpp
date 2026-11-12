/**
 * @file planet_grid.cpp
 * @brief See planet_grid.h. TRANSITIONAL: moves to the planet project.
 */

#include <toyengine/world/planet_grid.h>

#include <algorithm>
#include <cmath>

#include <toyengine/world/planet.h>
#include <toyengine/world/terra_data.h>
#include <toyengine/world/terrain_params.h>

namespace toy {
namespace world {

glm::ivec2 PlanetGrid::neighbour(std::int32_t x, std::int32_t y,
                                     std::int32_t dx, std::int32_t dy) const {
    // The layout's own stepping, which turns onto the next face at an edge.
    return layout_.neighbour(x, y, dx, dy);
}

glm::ivec2 PlanetGrid::tile_from(std::int32_t base_x, std::int32_t base_y,
                                     std::int32_t lx, std::int32_t ly) const {
    // Resolved from the CHUNK's face, not from the tile: a coordinate one past a face's edge
    // belongs to either of the two faces meeting there, and picking the other one skips the
    // turn that joining it requires. See TileGrid::tile_from().
    const std::int32_t face = base_x / layout_.n;
    const std::int32_t face_x = base_x - face * layout_.n;
    return layout_.wrap(face, face_x + lx, base_y + ly);
}

bool PlanetGrid::tile_at(const TerrainParams& params, const glm::vec3& local,
                            glm::ivec2& tile) const {
    if (glm::length(local) < 1e-4f) return false;
    tile = params.layout().tile_of(glm::normalize(glm::dvec3(local)));
    return true;
}

IRect PlanetGrid::clip_brush(const TerrainParams& params, const IRect& rect,
                                 const glm::ivec2& at) const {
    const int n = std::max(1, params.face_tiles);
    const int face = std::clamp(at.x / n, 0, 5);
    return IRect{std::max(rect.x0, face * n), std::max(rect.y0, 0),
                 std::min(rect.x1, (face + 1) * n), std::min(rect.y1, n)};
}

int PlanetGrid::region_at(const TerrainParams& params, std::int32_t x_tile) const {
    return std::clamp(x_tile / std::max(1, params.face_tiles), 0, 5);
}

glm::vec3 PlanetGrid::grid_point(const TerrainParams& params, double x_tiles, double y_tiles,
                                     double height, int region) const {
    const PlanetLayout layout = params.layout();
    const std::int32_t n = std::max(1, params.face_tiles);
    const std::int32_t face =
        region >= 0 ? std::clamp(region, 0, 5)
                    : std::clamp(static_cast<std::int32_t>(x_tiles) / n, 0, 5);
    const double x = (x_tiles - static_cast<double>(face) * n) * params.tile_size;
    const double y = y_tiles * params.tile_size;
    return glm::vec3(layout.to_planet(face, x, y, height));
}

bool PlanetGrid::raycast(const TerraData& data, const TerrainParams& params,
                             const glm::vec3& origin, const glm::vec3& dir, float /*max_t*/,
                             TileHit& hit) const {
    const PlanetLayout layout = params.layout();
    const double r0 = layout.radius();
    const glm::dvec3 o(origin), v(glm::normalize(glm::dvec3(dir)));
    const auto top = [&](const glm::dvec3& p, glm::ivec2& tile) {
        tile = layout.tile_of(glm::normalize(p));
        return r0 + static_cast<double>(std::max<int>(0, data.get(tile.x, tile.y).steps)) * params.height_step;
    };
    const double outer = r0 + 256.0 * params.height_step;
    // Where the ray meets the outer sphere (|o + t v| = outer).
    const double b = glm::dot(o, v), c = glm::dot(o, o) - outer * outer;
    const double disc = b * b - c;
    if (disc < 0.0) return false;
    double t = std::max(0.0, -b - std::sqrt(disc));
    const double t_end = -b + std::sqrt(disc);
    const double step = 0.25 * params.tile_size;
    glm::ivec2 tile;
    double prev = t;
    for (; t <= t_end; t += step) {
        const glm::dvec3 p = o + v * t;
        if (glm::length(p) <= top(p, tile)) {
            double lo = prev, hi = t;
            for (int i = 0; i < 20; ++i) {
                const double mid = 0.5 * (lo + hi);
                const glm::dvec3 q = o + v * mid;
                glm::ivec2 tq;
                if (glm::length(q) <= top(q, tq)) hi = mid; else lo = mid;
            }
            const glm::dvec3 at = o + v * hi;
            top(at, hit.tile);
            hit.point = glm::vec3(at);
            hit.normal = glm::vec3(glm::normalize(at));
            hit.t = static_cast<float>(hi);
            return true;
        }
        prev = t;
    }
    return false;
}

void PlanetGrid::world_bounds(const TerrainParams& params, float relief, glm::vec3& centre,
                                  float& radius) const {
    centre = glm::vec3(0.0f);
    radius = params.layout().radius() + std::max(relief, 0.0f);
}

} // namespace world
} // namespace toy
