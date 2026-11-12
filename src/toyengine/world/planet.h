/**
 * @file planet.h
 * @brief A planet TileTerrain's layout: six square faces of tiles on a cube, bent onto a sphere.
 *
 * A planet's tiles are the flat terrain's tiles, six times over. Each face of a cube is an
 * `N x N` grid (`face_tiles`), meshed exactly as a flat chunk is -- in **face space**: x and y
 * across the face in world units (`0 .. N * tile_size`), z the column height -- and then each
 * vertex is bent onto the planet by `to_planet()`: `direction(face, x, y) * (radius + z)`.
 *
 * - **Equal-angle faces.** A face's x and y are angles, not plane coordinates (`tan(x * pi/4)`),
 *   so tiles stay near-square everywhere; a gnomonic cube would make its corner tiles half the
 *   size of its middle ones.
 * - **Radius.** A face spans a quarter of the way round, so `radius = 2 N tile_size / pi`: a tile
 *   in the middle of a face is `tile_size` across, as on a flat terrain.
 * - **Storage.** The six faces sit side by side in one `6N x N` TileMapData -- face f is columns
 *   `[f N, (f+1) N)` -- so a planet's `.tiles` file is the flat format.
 * - **Neighbours.** Across a face's edge, the next tile is on another face, turned: `wrap()`
 *   finds it by stepping off the face in face space and looking the direction up again. The
 *   cube's eight corners, where three faces meet, have no fourth tile; the diagonal there resolves
 *   to one of the two that touch it.
 *
 * Faces, outward axis then the face's x and y axes (each right-handed: x cross y = out):
 * +X (y, z), -X (-y, z), +Y (-x, z), -Y (x, z), +Z (y, -x), -Z (y, x).
 */

#ifndef TOYENGINE_WORLD_PLANET_H
#define TOYENGINE_WORLD_PLANET_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace toy {
namespace world {

/**
 * @struct PlanetLayout
 * @brief The cube-sphere a planet terrain is laid on; see the file doc.
 */
struct PlanetLayout {
    std::int32_t n = 128;     ///< Tiles along one face's edge.
    float tile_size = 1.0f;   ///< Width of a tile in the middle of a face.

    static constexpr double k_pi = 3.14159265358979323846;

    /** @brief The radius height 0 lies at. */
    float radius() const { return static_cast<float>(2.0 * n * tile_size / k_pi); }

    /** @brief A face's outward, x and y axes. */
    static void axes(std::int32_t face, glm::dvec3& out, glm::dvec3& ax, glm::dvec3& ay) {
        switch (face) {
            case 0:  out = {1, 0, 0};  ax = {0, 1, 0};  ay = {0, 0, 1};  return;
            case 1:  out = {-1, 0, 0}; ax = {0, -1, 0}; ay = {0, 0, 1};  return;
            case 2:  out = {0, 1, 0};  ax = {-1, 0, 0}; ay = {0, 0, 1};  return;
            case 3:  out = {0, -1, 0}; ax = {1, 0, 0};  ay = {0, 0, 1};  return;
            case 4:  out = {0, 0, 1};  ax = {0, 1, 0};  ay = {-1, 0, 0}; return;
            default: out = {0, 0, -1}; ax = {0, 1, 0};  ay = {1, 0, 0};  return;
        }
    }

    /**
     * @brief The unit direction of face-space point `(x, y)` (world units across the face,
     *        `0 .. N * tile_size`). Valid a little past the face's edges too -- that is how a
     *        neighbour across an edge is found.
     */
    glm::dvec3 direction(std::int32_t face, double x, double y) const {
        glm::dvec3 out, ax, ay;
        axes(face, out, ax, ay);
        const double span = static_cast<double>(n) * tile_size;
        const double u = std::tan((2.0 * x / span - 1.0) * k_pi * 0.25);
        const double v = std::tan((2.0 * y / span - 1.0) * k_pi * 0.25);
        return glm::normalize(out + ax * u + ay * v);
    }

    /** @brief A face-space position (x, y across the face, z height) on the planet, centre-relative. */
    glm::dvec3 to_planet(std::int32_t face, double x, double y, double z) const {
        return direction(face, x, y) * (static_cast<double>(radius()) + z);
    }

    /** @brief The face a direction points through, and its face-space x and y there. */
    void locate(const glm::dvec3& dir, std::int32_t& face, double& x, double& y) const {
        const double ax = std::abs(dir.x), ay = std::abs(dir.y), az = std::abs(dir.z);
        if (ax >= ay && ax >= az) face = dir.x >= 0.0 ? 0 : 1;
        else if (ay >= az)        face = dir.y >= 0.0 ? 2 : 3;
        else                      face = dir.z >= 0.0 ? 4 : 5;
        glm::dvec3 out, fx, fy;
        axes(face, out, fx, fy);
        const double along = glm::dot(dir, out);
        const double u = glm::dot(dir, fx) / along;
        const double v = glm::dot(dir, fy) / along;
        const double span = static_cast<double>(n) * tile_size;
        x = (std::atan(u) / (k_pi * 0.25) + 1.0) * 0.5 * span;
        y = (std::atan(v) / (k_pi * 0.25) + 1.0) * 0.5 * span;
    }

    /** @brief The tile a direction points through: storage column / row (face f is columns f N..). */
    glm::ivec2 tile_of(const glm::dvec3& dir) const {
        std::int32_t face = 0;
        double x = 0.0, y = 0.0;
        locate(dir, face, x, y);
        const std::int32_t i = std::clamp(static_cast<std::int32_t>(std::floor(x / tile_size)), 0, n - 1);
        const std::int32_t j = std::clamp(static_cast<std::int32_t>(std::floor(y / tile_size)), 0, n - 1);
        return {face * n + i, j};
    }

    /** @brief A tile's centre direction, by storage column / row. */
    glm::dvec3 tile_direction(std::int32_t column, std::int32_t row) const {
        const std::int32_t face = std::clamp(column / n, 0, 5);
        const std::int32_t i = column - face * n;
        return direction(face, (i + 0.5) * tile_size, (row + 0.5) * tile_size);
    }

    /**
     * @brief The stored tile at face-local `(i, j)` of `face`, where `(i, j)` may lie a tile or two
     *        past the face's edge -- on the neighbouring face, then.
     */
    glm::ivec2 wrap(std::int32_t face, std::int32_t i, std::int32_t j) const {
        if (i >= 0 && j >= 0 && i < n && j < n) return {face * n + i, j};
        return tile_of(direction(face, (i + 0.5) * tile_size, (j + 0.5) * tile_size));
    }

    /** @brief The stored tile one step from stored tile `(column, row)` along `(dx, dy)` in its face. */
    glm::ivec2 neighbour(std::int32_t column, std::int32_t row, std::int32_t dx, std::int32_t dy) const {
        const std::int32_t face = std::clamp(column / n, 0, 5);
        return wrap(face, column - face * n + dx, row + dy);
    }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PLANET_H
