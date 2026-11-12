/**
 * @file world_frame.h
 * @brief Which way is up, and how high up you are: the one authoritative answer for a scene,
 *        whether its world is a flat Z-up plane or a curved one.
 *
 * The engine is Z-up, and nearly every world-impacting system was written for that: an
 * altitude is a `p.z`, a wind is an XY bearing, a ground is a plane, a sky's zenith is +Z.
 * A curved world -- one whose surface wraps around a centre, so up points away from it and
 * differs at every point -- breaks all of it at once.
 *
 * This is the value those systems ask instead of reading `.z`. A default-constructed
 * `WorldFrame` **is** the flat world: `up_at()` returns +Z and `altitude_at()` returns
 * `p.z`, so a caller writes one expression and a flat scene keeps its old answer. The
 * pattern is `physxcoopa`'s `MoveOptions::up` (`character_motor.h`) -- carry an up, default
 * it to +Z, and keep an exact fast path for +Z so the flat world's arithmetic is unchanged.
 *
 * It is a plain POD with no owner, like `EnvSource` beside it: a curved world's terrain
 * publishes one per frame through `Engine::set_world_frame_source()`, and `render/`,
 * `weather/` and the editor read copies. That is why it lives here in `env/` -- the glm-only
 * leaf layer both the renderer and the weather already depend on -- rather than in `world/`,
 * which neither may include. Being a value rather than an interface is what lets it be
 * copied into `render::WorldFrameState`, `CloudState` and a pass's `Params` without any
 * lifetime contract across the render thread.
 *
 * **Not** a replacement for `coopa::physx::PhysicsWorld::up_at()`. That answers from the
 * gravity field, which is the right source for anything already holding a physics world (a
 * character's feet, a vehicle's suspension) and keeps working for gravity that no world
 * shape explains. This answers from geometry, for everything that has no physics world to
 * ask and for the altitude question the gravity field cannot answer at all.
 *
 * The GPU twin is `assets/shaders/gfx/world_frame.glsl`, which carries the same value as a
 * `vec4` (`xyz` centre, `w` radius) under the same rule: `w <= 0` is the flat world.
 */

#ifndef TOYENGINE_ENV_WORLD_FRAME_H
#define TOYENGINE_ENV_WORLD_FRAME_H

#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace env {

/** @brief World up on a flat Z-up world, and the pole a curved world's tangent basis is built on. */
inline const glm::vec3 k_world_up{0.0f, 0.0f, 1.0f};

/**
 * @struct WorldFrame
 * @brief The scene's world shape: curved about `centre` when `radius > 0`, flat otherwise.
 *        See the file doc.
 *
 * Trivially copyable, and cheap to ask: every query is one `radius > 0` compare on a value
 * already in a register, not a branch at the call site.
 */
struct WorldFrame {
    /** @brief The centre the world curves about, world space. Unused on a flat world. */
    glm::vec3 centre{0.0f};

    /**
     * @brief The radius height 0 lies at (the surface's datum), world units.
     *
     * `<= 0` means a flat Z-up world -- the default, and what every flat scene publishes.
     */
    float radius = 0.0f;

    /** @brief True when this frame describes a curved world. */
    bool curved() const { return radius > 0.0f; }

    /**
     * @brief Unit up at a world point: away from the centre when curved, +Z on a flat world.
     *
     * At the exact centre there is no "down", so +Z is returned rather than a NaN.
     *
     * @param p World-space point.
     * @return Unit vector.
     */
    glm::vec3 up_at(const glm::vec3& p) const {
        if (!curved()) return k_world_up;
        const glm::vec3 d = p - centre;
        const float len = glm::length(d);
        return len > 1e-6f ? d / len : k_world_up;
    }

    /**
     * @brief Height above the datum: `length(p - centre) - radius` when curved, `p.z` flat.
     *
     * The quantity every `fog_height_base`, `cloud_altitude`, `ground_height` and sky
     * view-radius means by "height", stated so a curved world can answer it too.
     *
     * @param p World-space point.
     * @return Altitude in world units; negative below the datum.
     */
    float altitude_at(const glm::vec3& p) const {
        return curved() ? glm::length(p - centre) - radius : p.z;
    }

    /**
     * @brief A local basis at `p`: columns east, north, up. Identity on a flat world.
     *
     * North is the pole (+Z) flattened into the tangent plane, so on a curved world this is
     * the geographic frame a compass bearing (`wind_heading`) and a sun path are expressed in.
     * Within `~2.6` degrees of either pole the basis has no meaningful bearing, so a fixed
     * fallback is used -- the same degenerate-axis guard `ToyRenderPipeline::sky_frame_()`
     * makes for its antipodal case. Transport a vector with transport() rather than
     * re-deriving it from components near a pole.
     *
     * @param p World-space point.
     * @return Orthonormal, right-handed (`east x north == up`).
     */
    glm::mat3 tangent_at(const glm::vec3& p) const {
        if (!curved()) return glm::mat3(1.0f);
        const glm::vec3 up = up_at(p);
        glm::vec3 east = glm::cross(k_world_up, up);
        const float len = glm::length(east);
        // At a pole k_world_up and up are parallel: pick any axis perpendicular to up.
        east = len > 1e-3f ? east / len : glm::normalize(glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), up));
        return glm::mat3(east, glm::cross(up, east), up);
    }

    /**
     * @brief The world point at `altitude` above the datum in direction `dir` from the centre.
     *
     * @param dir      Direction from the centre; need not be normalized. On a flat world only
     *                 its xy is used, as the point's position in the plane.
     * @param altitude Height above the datum, world units.
     * @return World-space point.
     */
    glm::vec3 point_at(const glm::vec3& dir, float altitude) const {
        if (!curved()) return glm::vec3(dir.x, dir.y, altitude);
        const float len = glm::length(dir);
        const glm::vec3 d = len > 1e-6f ? dir / len : k_world_up;
        return centre + d * (radius + altitude);
    }
};

/**
 * @brief Carries a tangent vector from one point on the frame to another, keeping its length.
 *
 * Parallel transport: the vector is turned by the same rotation that takes the up at `from`
 * to the up at `to`, so a wind blowing along a coast still blows along it a mile further on
 * and never spins or shrinks. The alternative -- re-deriving the vector from an east/north
 * bearing at each point -- is what makes a compass wind pinwheel at the poles.
 *
 * This is the operation `CharacterController::advance_field_()` performs by hand on its own
 * frame; it is factored out here so the wind and the cloud deck share one implementation.
 *
 * @param frame     The frame; a flat one returns `v` unchanged.
 * @param v         Tangent vector at `from`, world space.
 * @param from      Where `v` is given.
 * @param to        Where it is wanted.
 * @return `v` at `to`.
 */
inline glm::vec3 transport(const WorldFrame& frame, const glm::vec3& v,
                           const glm::vec3& from, const glm::vec3& to) {
    if (!frame.curved()) return v;
    const glm::vec3 a = frame.up_at(from);
    const glm::vec3 b = frame.up_at(to);
    const glm::vec3 axis = glm::cross(a, b);
    const float sin_t = glm::length(axis);
    const float cos_t = glm::dot(a, b);
    if (sin_t < 1e-6f) {
        // Same up, or exactly antipodal: a turn is either unnecessary or undefined.
        return cos_t > 0.0f ? v : -v;
    }
    // Rodrigues' rotation by the angle between the two ups, about their common perpendicular.
    const glm::vec3 k = axis / sin_t;
    return v * cos_t + glm::cross(k, v) * sin_t + k * (glm::dot(k, v) * (1.0f - cos_t));
}

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_WORLD_FRAME_H
