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

#include <coopa/scene/scene.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/particles/particle_system.h>
#include <toyengine/render/visibility.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/water/water_system.h>
#include <toyengine/weather/weather_surface.h>

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
    int known_cells() const {
        int k = 0;
        for (char c : known_) k += c ? 1 : 0;
        return k;
    }

    /** @brief Forgets everything (the next update() starts over). */
    void reset() {
        known_.clear();
        heights_.clear();
        field_.reset();
        source_ = Source::None;
    }

    /**
     * @brief Moves the grid under `eye`, probes, and republishes field().
     * @param radius          The grid reaches at least this far from the eye (m).
     * @param fallback_splash Whether the fallback plane (where nothing else was found) takes splashes.
     */
    void update(coopa::scene::Scene& scene, const glm::vec3& eye, float fallback, float dt,
                float radius = 24.0f, bool fallback_splash = false) {
        cells = std::max(16, static_cast<int>(std::ceil(2.0f * radius / cell_size)) + 2);
        using coopa::physx::system::PhysicsSystem;
        auto* physics = dynamic_cast<PhysicsSystem*>(scene.find_system("Physics"));
        const Source src = scene.is_simulating() && physics ? Source::Physics : Source::Bounds;
        const int n = std::max(4, cells);
        const glm::ivec2 origin(static_cast<int>(std::floor(eye.x / cell_size)) - n / 2,
                                static_cast<int>(std::floor(eye.y / cell_size)) - n / 2);
        if (src != source_ || static_cast<int>(heights_.size()) != n * n || fallback != fallback_) {
            source_ = src;
            fallback_ = fallback;
            heights_.assign(static_cast<size_t>(n) * n, std::numeric_limits<float>::quiet_NaN());
            sky_.assign(static_cast<size_t>(n) * n, std::numeric_limits<float>::quiet_NaN());
            normals_.assign(static_cast<size_t>(n) * n, glm::vec3(0.0f, 0.0f, 1.0f));
            splash_.assign(static_cast<size_t>(n) * n, 0);
            known_.assign(static_cast<size_t>(n) * n, 0);
            origin_ = origin;
            bounds_timer_ = 0.0f;
        } else if (origin != origin_) {
            scroll_(origin, n);
        }

        if (src == Source::Physics) probe_physics_(*physics, eye, n);
        else {
            bounds_timer_ -= dt;
            if (bounds_timer_ <= 0.0f) { probe_bounds_(scene, n); bounds_timer_ = 0.25f; }
        }

        auto f = std::make_shared<particles::GroundField>();
        f->origin = glm::vec2(origin_) * cell_size;
        f->cell = cell_size;
        f->nx = f->ny = n;
        f->fallback = fallback;
        f->heights = heights_;
        f->sky_heights = sky_;
        f->normals = normals_;
        f->splash = splash_;
        f->fallback_splash = fallback_splash;
        // Where nothing was found the fallback plane is the surface: it splashes as asked.
        for (size_t k = 0; k < heights_.size(); ++k) if (std::isnan(heights_[k])) f->splash[k] = fallback_splash || splash_[k] ? 1 : 0;
        // Water surfaces: re-sampled a few times a second (or when the grid moves), not every frame.
        water_timer_ -= dt;
        if (water_timer_ <= 0.0f || water_origin_ != origin_ || water_.size() != heights_.size()) {
            water_.assign(heights_.size(), std::numeric_limits<float>::quiet_NaN());
            water_normals_.assign(heights_.size(), glm::vec3(0.0f, 0.0f, 1.0f));
            water_splash_.assign(heights_.size(), 0);
            if (auto* water = dynamic_cast<water::WaterSystem*>(scene.find_system("Water"))) sample_water_(*water, f->origin, n);
            water_origin_ = origin_;
            water_timer_ = 0.2f;
        }
        for (size_t k = 0; k < water_.size(); ++k) {
            if (std::isnan(water_[k])) continue;
            float& h = f->heights[k];
            if (water_[k] > (std::isnan(h) ? fallback : h)) {
                h = water_[k];
                f->normals[k] = water_normals_[k];
                f->splash[k] = water_splash_[k];
            }
            float& sh = f->sky_heights[k];
            if (water_[k] > (std::isnan(sh) ? fallback : sh)) sh = water_[k];
        }
        field_ = std::move(f);
    }

private:
    void scroll_(const glm::ivec2& origin, int n) {
        std::vector<float> h(static_cast<size_t>(n) * n, std::numeric_limits<float>::quiet_NaN());
        std::vector<float> sk(static_cast<size_t>(n) * n, std::numeric_limits<float>::quiet_NaN());
        std::vector<glm::vec3> nm(static_cast<size_t>(n) * n, glm::vec3(0.0f, 0.0f, 1.0f));
        std::vector<uint8_t> sp(static_cast<size_t>(n) * n, 0);
        std::vector<char> k(static_cast<size_t>(n) * n, 0);
        const glm::ivec2 d = origin - origin_;
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int oi = i + d.x, oj = j + d.y;
                if (oi < 0 || oj < 0 || oi >= n || oj >= n) continue;
                h[static_cast<size_t>(j) * n + i] = heights_[static_cast<size_t>(oj) * n + oi];
                sk[static_cast<size_t>(j) * n + i] = sky_[static_cast<size_t>(oj) * n + oi];
                nm[static_cast<size_t>(j) * n + i] = normals_[static_cast<size_t>(oj) * n + oi];
                sp[static_cast<size_t>(j) * n + i] = splash_[static_cast<size_t>(oj) * n + oi];
                k[static_cast<size_t>(j) * n + i] = known_[static_cast<size_t>(oj) * n + oi];
            }
        }
        heights_.swap(h);
        sky_.swap(sk);
        normals_.swap(nm);
        splash_.swap(sp);
        known_.swap(k);
        origin_ = origin;
    }

    void probe_physics_(const coopa::physx::system::PhysicsSystem& physics, const glm::vec3& eye, int n) {
        int budget = std::max(1, rays_per_frame);
        auto probe = [&](size_t idx) {
            const int i = static_cast<int>(idx % static_cast<size_t>(n)), j = static_cast<int>(idx / static_cast<size_t>(n));
            coopa::physx::geometry::Ray ray;
            ray.origin = glm::vec3((static_cast<float>(origin_.x + i) + 0.5f) * cell_size,
                                   (static_cast<float>(origin_.y + j) + 0.5f) * cell_size, eye.z + probe_height);
            ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
            ray.max_distance = probe_height * 4.0f + 200.0f;
            coopa::physx::system::PhysicsSystem::RaycastHit hit;
            const bool found = physics.raycast(ray, hit, ~0u, /*include_triggers=*/false);
            heights_[idx] = found ? hit.point.z : std::numeric_limits<float>::quiet_NaN();
            // The sky layer looks through moving bodies (a sled, a crate in flight) to the first
            // surface without a Rigidbody: lying snow must not vanish under whatever passes over it.
            {
                bool sky_found = found;
                coopa::physx::system::PhysicsSystem::RaycastHit sky_hit = hit;
                for (int pass = 0; pass < 3 && sky_found && moving_(sky_hit.object); ++pass) {
                    coopa::physx::geometry::Ray below = ray;
                    below.origin.z = sky_hit.point.z - 0.02f;
                    below.max_distance = std::max(0.0f, ray.max_distance - (ray.origin.z - below.origin.z));
                    sky_found = physics.raycast(below, sky_hit, ~0u, /*include_triggers=*/false);
                }
                sky_[idx] = sky_found ? sky_hit.point.z : std::numeric_limits<float>::quiet_NaN();
            }
            normals_[idx] = found && hit.normal.z > 0.05f ? glm::normalize(hit.normal) : glm::vec3(0.0f, 0.0f, 1.0f);
            splash_[idx] = found && takes_splashes(hit.object) ? 1 : 0;
            known_[idx] = 1;
            --budget;
        };
        // New cells first, nearest the middle outward (a ring scan is overkill: rows from the centre).
        const size_t total = static_cast<size_t>(n) * n;
        for (int r = 0; r <= n / 2 && budget > 0; ++r) {
            for (int j = n / 2 - r; j <= n / 2 + r && budget > 0; ++j) {
                for (int i = n / 2 - r; i <= n / 2 + r && budget > 0; ++i) {
                    if (j < 0 || i < 0 || j >= n || i >= n) continue;
                    if (std::max(std::abs(i - n / 2), std::abs(j - n / 2)) != r) continue;   // this ring only
                    const size_t idx = static_cast<size_t>(j) * n + i;
                    if (!known_[idx]) probe(idx);
                }
            }
        }
        // Then refresh a slice round-robin (doors open, crates move).
        for (int k = 0; k < std::max(1, rays_per_frame / 8) && budget > 0; ++k) {
            refresh_cursor_ = (refresh_cursor_ + 1) % total;
            probe(refresh_cursor_);
        }
    }

    void probe_bounds_(coopa::scene::Scene& scene, int n) {
        using coopa::gfx::engine::components::MeshRenderer;
        std::fill(heights_.begin(), heights_.end(), std::numeric_limits<float>::quiet_NaN());
        std::fill(sky_.begin(), sky_.end(), std::numeric_limits<float>::quiet_NaN());
        std::fill(normals_.begin(), normals_.end(), glm::vec3(0.0f, 0.0f, 1.0f));   // bounds: flat tops
        std::fill(splash_.begin(), splash_.end(), uint8_t(0));
        const float size = static_cast<float>(n) * cell_size;
        const glm::vec2 lo(glm::vec2(origin_) * cell_size);
        for (MeshRenderer* mr : scene.get_components<MeshRenderer>()) {
            if (!mr->owner || !mr->owner->active() || toy::scene::is_runtime_object(*mr->owner)) continue;
            const auto* mesh = mr->get_mesh().get();
            auto* tc = mr->owner->get_transform();
            if (!mesh || !tc) continue;
            const render::WorldBounds b = render::world_aabb(tc->transform().get_world_matrix(), mesh->bounds_min(), mesh->bounds_max());
            const bool splashes = takes_splashes(mr->owner);
            const bool moving = moving_(mr->owner);
            const float top = b.center.z + b.extent.z;
            const int i0 = std::max(0, static_cast<int>(std::floor((b.center.x - b.extent.x - lo.x) / cell_size)));
            const int i1 = std::min(n - 1, static_cast<int>(std::floor((b.center.x + b.extent.x - lo.x) / cell_size)));
            const int j0 = std::max(0, static_cast<int>(std::floor((b.center.y - b.extent.y - lo.y) / cell_size)));
            const int j1 = std::min(n - 1, static_cast<int>(std::floor((b.center.y + b.extent.y - lo.y) / cell_size)));
            // Huge footprints (ground planes, terrain chunks): their box top is no surface, so the
            // fallback plane stands in for their height -- but they still say whether it splashes.
            const bool huge = b.extent.x * 2.0f > size * 0.5f || b.extent.y * 2.0f > size * 0.5f;
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const size_t k = static_cast<size_t>(j) * n + i;
                    if (huge) { if (splashes && std::isnan(heights_[k])) splash_[k] = 1; continue; }
                    float& h = heights_[k];
                    if (std::isnan(h) || top > h) { h = top; splash_[k] = splashes ? 1 : 0; }
                    float& sh = sky_[k];
                    if (!moving && (std::isnan(sh) || top > sh)) sh = top;
                }
            }
        }
        std::fill(known_.begin(), known_.end(), 1);
    }

    void sample_water_(const water::WaterSystem& water, const glm::vec2& origin, int n) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                water::WaterSample s;
                const water::WaterBody* body = nullptr;
                if (!water.sample(origin + (glm::vec2(i, j) + 0.5f) * cell_size, s, &body)) continue;
                water_[static_cast<size_t>(j) * n + i] = s.surface_height;
                water_normals_[static_cast<size_t>(j) * n + i] = s.normal;
                water_splash_[static_cast<size_t>(j) * n + i] = body && takes_splashes(body->owner) ? 1 : 0;
            }
        }
    }

    /** @brief True if `obj` moves (a Rigidbody on it or an ancestor): the sky layer ignores it. */
    static bool moving_(const coopa::scene::SceneObject* obj) {
        for (const coopa::scene::SceneObject* o = obj; o; o = o->parent()) {
            if (const_cast<coopa::scene::SceneObject*>(o)->get_component<coopa::physx::components::RigidbodyComponent>()) return true;
        }
        return false;
    }

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
