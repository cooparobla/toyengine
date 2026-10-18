/**
 * @file destructible.h
 * @brief Marks an object as breakable: it shatters into physical chunks, by a hard enough hit
 *        or by a call.
 *
 * The scene-authored half of destruction. The work is toy::scene::DestructionSystem's (see
 * destruction_system.h): it fractures the object's mesh on a worker, and when the break comes
 * it swaps the object for one rigid body per chunk. This component is the settings and the
 * switch -- no geometry, no physics, no GPU, so it stays a plain data component that a tool
 * can edit and a test can drive.
 *
 * The chunks are Voronoi cells of the source mesh (toyengine/destruct/fracture.h), so they fit
 * back together exactly: what you see immediately after a break is the mesh you had, in pieces,
 * before the pieces have moved.
 *
 * Example YAML:
 * @code
 * - type: Destructible
 *   pieces: 16            # cells to break into
 *   seed: 3               # the same seed always breaks the same way
 *   impact_impulse: 800   # N*s of impact that breaks it (mass x speed); 0 = only shatter()
 *   scatter: 1.5          # outward speed given to the chunks (m/s)
 *   chunk_lifetime: 10.0  # seconds before the debris is cleaned up; 0 keeps it
 * @endcode
 */

#ifndef TOYENGINE_SCENE_DESTRUCTIBLE_H
#define TOYENGINE_SCENE_DESTRUCTIBLE_H

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>

namespace toy {
namespace scene {

/**
 * @class Destructible
 * @brief An object that breaks into chunks.
 *
 * Put it on an object with a MeshRenderer; that mesh is what gets fractured. A Rigidbody is not
 * required -- a wall standing as a static collider breaks just as well as a crate flying
 * through the air -- but the hit that breaks it has to come from somewhere, so an object with
 * no collider can only be broken by shatter().
 */
class Destructible : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Destructible"; }

    /** @brief Cells the mesh is cut into. Pieces that come out empty are dropped, so the chunk
     *         count can be lower -- see fracture.h. */
    int pieces = 12;

    /** @brief Seeds the break. The same mesh, pieces and seed always break identically. */
    std::uint32_t seed = 1u;

    /** @brief How uneven the chunks are, 0..1: 0 is a cast slab, 1 is a rock. */
    float irregularity = 0.7f;

    /**
     * @brief How hard a hit has to be to break this, as the contact's impulse in N*s; 0 never
     *        breaks on impact.
     *
     * The impulse, NOT a speed, because a speed is not what a collision can report: by the
     * time Collider::on_collision_enter fires, the solver has already stopped the thing that
     * hit, and its "relative velocity" is the leftover -- a 120 kg weight arriving at 10 m/s
     * reads as 1.9 m/s. The impulse is that solve's own output and measures exactly what was
     * wanted: mass times the speed it lost. Measured in this scene, a weight landing hard
     * delivers 1300-2500 while settling contacts stay under 420, so thresholds are not
     * delicate.
     *
     * Rule of thumb: a mass of `m` kg arriving at `v` m/s delivers about `m * v`.
     */
    float impact_impulse = 800.0f;

    /** @brief Outward speed given to each chunk as it is born, m/s -- the "pop" of the break. */
    float scatter = 1.5f;

    /** @brief Share of the blow's speed the chunks carry on with, 0..1. */
    float impact_transfer = 0.35f;

    /** @brief Seconds a chunk lives before it is cleaned up; 0 keeps the debris for ever. */
    float chunk_lifetime = 10.0f;

    /** @brief Seconds a chunk spends sinking out of sight before it goes; 0 makes it vanish. */
    float chunk_fade = 1.0f;

    /** @brief Density used to weigh a chunk from its volume, kg/m^3 (oak is ~700, concrete ~2400). */
    float chunk_density = 700.0f;

    /** @brief Most chunks to spawn (the largest are kept); 0 spawns every piece. */
    int max_chunks = 0;

    /** @brief physics_materials/<name>.yaml for the chunks; empty uses the engine default. */
    std::string chunk_physics_material;

    /**
     * @brief Fracture as soon as the scene starts rather than at the moment of the break.
     *
     * Cutting a mesh into cells costs milliseconds, and the frame something explodes is the
     * worst frame to spend them in. On by default: the fracture runs on a worker during the
     * quiet time after load, and the break itself is then only spawning bodies. Off saves the
     * memory of pieces that may never be used -- worth it for a hundred crates, not for five.
     */
    bool prefracture = true;

    /**
     * @brief How much of its box a chunk must fill before it collides as that one box, 0..1.
     *
     * A Voronoi shard is as often a wedge as a brick, and a wedge fills barely half of any box
     * drawn round it -- as collision that reads as debris resting on thin air at the corners.
     * Below this ratio the chunk is cut along its longest axis and collides as several boxes
     * (child colliders, gathered into one compound body). 1 always splits, 0 never does.
     */
    float collider_fill = 0.6f;

    /** @brief Most collision boxes one chunk may use. 1 keeps every chunk a single box. */
    int max_boxes = 4;

    /**
     * @brief Listens for impacts on the owner's collider, so a hard enough hit breaks it.
     *
     * Connected here rather than polled by the system because the contact that matters exists
     * for one step inside the solve: Collider::on_collision_enter carries the relative
     * velocity of the pair, which is the thing `impact_speed` is a threshold on, and by the
     * next frame it is gone.
     */
    void start() override;

    /** @brief Breaks it now, if it has not broken already. */
    void shatter() { shatter(glm::vec3(0.0f), glm::vec3(0.0f)); }

    /**
     * @brief Breaks it now, throwing the chunks away from `point` and along `impulse`.
     *
     * @param point   Where the blow landed, in world space; chunks are pushed away from it.
     * @param impulse The blow's direction and strength (m/s, scaled by `impact_transfer`).
     */
    void shatter(const glm::vec3& point, const glm::vec3& impulse) {
        if (shattered_) return;
        pending_ = true;
        hit_point_ = point;
        hit_impulse_ = impulse;
    }

    /** @brief Whether the break has already happened. */
    bool shattered() const { return shattered_; }

    /** @brief How many chunks the break produced (0 until it breaks). */
    int chunk_count() const { return chunk_count_; }

    // --- DestructionSystem's own state; not for scenes to set ---

    /** @brief A break is waiting to be carried out on the next system pass. */
    bool pending() const { return pending_; }
    const glm::vec3& hit_point() const { return hit_point_; }
    const glm::vec3& hit_impulse() const { return hit_impulse_; }
    /** @brief Called by the system once it has spawned the chunks. */
    void mark_shattered(int chunks) {
        pending_ = false;
        shattered_ = true;
        chunk_count_ = chunks;
    }

private:
    coopa::event::ScopedConnection impact_;   /**< The collider signal; dropped with the component. */
    bool      pending_ = false;
    bool      shattered_ = false;
    int       chunk_count_ = 0;
    glm::vec3 hit_point_{0.0f};
    glm::vec3 hit_impulse_{0.0f};
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_DESTRUCTIBLE_H
