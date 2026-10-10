/**
 * @file ground_probe.h
 * @brief The precipitation height map around the viewer: the highest surface under each cell
 *        of a grid that follows the camera -- roofs, terrain, props, water surfaces -- for rain
 *        and snow to stop on (particles::GroundField). What makes rain land on a roof instead
 *        of falling through it into the house, and splash where it lands.
 *
 * Sources, per cell:
 *  - Physics (while the scene simulates and has a PhysicsSystem): a raycast straight down from
 *    above the camera against solid colliders (triggers ignored). Incremental: cells that scroll
 *    into the grid as the camera moves are probed first, then a slow round-robin refresh catches
 *    things that move; at most `rays_per_frame` a frame. Cells not yet probed use the fallback.
 *  - Bounds (edit mode, or no physics): the tops of MeshRenderer world bounds, rebuilt a few
 *    times a second. An approximation (a pitched roof is flat at its ridge) good enough for the
 *    editor preview; very large meshes (ground planes, terrain chunks) are skipped and left to
 *    the fallback plane.
 *  - Water (either way): a water surface above whatever the cell found wins, so rain splashes on
 *    a lake rather than on its bed.
 * Anything not found falls back to the weather's ground_height plane.
 *
 * Each cell also keeps the surface's normal (splashes lie in a sloped roof) and whether it takes
 * splashes -- the object hit, or an ancestor, carries a WeatherSurface (weather_surface.h).
 * Everything else takes the drops silently. The grid is sized to the farthest radius anything
 * asks for (WeatherDistantLandings), so distant splashes have surfaces to land on.
 */

#ifndef TOYENGINE_WEATHER_GROUND_PROBE_H
#define TOYENGINE_WEATHER_GROUND_PROBE_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <glm/glm.hpp>


#include <toyengine/particles/particle_system.h>
#include <toyengine/water/water_system.h>

namespace toy {
namespace weather {

class GroundProbe {
public:
    int cells = 48;                ///< Grid cells a side (the grid is cells * cell_size metres wide).
    float cell_size = 1.0f;
    int rays_per_frame = 400;
    float probe_height = 60.0f;    ///< Rays start this far above the camera.

    enum class Source { None, Physics, Bounds };

    /** @brief The current height map (null until the first update()). */
    const std::shared_ptr<const particles::GroundField>& field() const { return field_; }
    Source source() const { return source_; }
    /** @brief Cells probed so far in the current grid (tests / diagnostics). */
    int known_cells() const;

    /** @brief Forgets everything (the next update() starts over). */
    void reset();

    /**
     * @brief Moves the grid under `eye`, probes, and republishes field().
     * @param radius          The grid reaches at least this far from the eye (m).
     * @param fallback_splash Whether the fallback plane (where nothing else was found) takes splashes.
     */
    void update(coopa::scene::Scene& scene, const glm::vec3& eye, float fallback, float dt,
                float radius = 24.0f, bool fallback_splash = false);

private:
    void scroll_(const glm::ivec2& origin, int n);

    void probe_physics_(const coopa::physx::system::PhysicsSystem& physics, const glm::vec3& eye, int n);

    void probe_bounds_(coopa::scene::Scene& scene, int n);

    void sample_water_(const water::WaterSystem& water, const glm::vec2& origin, int n);

    /** @brief True if `obj` moves (a Rigidbody on it or an ancestor): the sky layer ignores it. */
    static bool moving_(const coopa::scene::SceneObject* obj);

    std::vector<float> heights_;
    std::vector<float> sky_;   ///< Like heights_, ignoring moving bodies (see moving_()).
    std::vector<glm::vec3> normals_;
    std::vector<glm::vec3> water_normals_;
    std::vector<uint8_t> splash_, water_splash_;
    std::vector<char> known_;
    glm::ivec2 origin_{0};
    float fallback_ = 0.0f;
    Source source_ = Source::None;
    float bounds_timer_ = 0.0f;
    size_t refresh_cursor_ = 0;
    std::vector<float> water_;
    glm::ivec2 water_origin_{std::numeric_limits<int>::min()};
    float water_timer_ = 0.0f;
    std::shared_ptr<const particles::GroundField> field_;
};

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_GROUND_PROBE_H
