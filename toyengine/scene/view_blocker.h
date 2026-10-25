/**
 * @file view_blocker.h
 * @brief A wall that dissolves itself out of the way when its collider blocks the view of an
 *        XraySubject, and comes back when it stops.
 *
 * The dissolve is an alpha-test erosion, not transparency: the material stays CUTOUT and its
 * `alpha_cutoff` rises, so a noise mask eats the wall away in speckles and the wall is never
 * translucent glass.
 *
 * By default the wall KEEPS casting its solid shadow while it erodes (`keep_shadow`), because
 * it has not actually gone anywhere. The shadow backbone (gfx/surface/shadow_fs.glsl) runs the
 * same alpha-mask test the G-buffer does, so a dissolving caster would otherwise lose its
 * shadow too; the renderer pushes a separate cutoff for the shadow views to prevent that.
 *
 * The material must already be CUTOUT with a noise mask in its alpha channel. Authoring it
 * that way rather than switching `alpha_mode` at runtime is deliberate: flipping the mode
 * changes the material's texture key and allocates a fresh descriptor set out of a pool
 * capped at 64. At rest the cutoff is 0, which leaves the wall fully solid.
 *
 * @code
 * - name: wall_0
 *   components:
 *     - type: MeshRenderer
 *       mesh: cube.000
 *       material:
 *         alpha_mode: CUTOUT
 *         alpha_cutoff: 0.0              # the system owns this from here on
 *         texture_alpha_mask: textures/dissolve_noise.png
 *     - type: BoxCollider
 *       size: { x: 4.0, y: 0.4, z: 3.0 }
 *     - type: ViewBlockerFade
 *       fade_in_time: 0.18
 *       fade_out_time: 0.35
 * @endcode
 *
 * Driven by `ViewBlockerSystem` (view_blocker_system.h), which does ONE capsule overlap from
 * the camera to the subject per frame and marks whichever of these it hit. A scene that never
 * installs that system leaves every wall solid.
 */

#ifndef TOYENGINE_SCENE_VIEW_BLOCKER_H
#define TOYENGINE_SCENE_VIEW_BLOCKER_H

#include <coopa/scene/component.h>

namespace toy {
namespace scene {

/**
 * @brief Moves a dissolve level toward where the blocking state wants it.
 *
 * Linear in time rather than exponential, so `fade_in_time` and `fade_out_time` mean what
 * they say: the number of seconds a full 0 -> `fade_target` dissolve takes. An exponential
 * ease never actually arrives, which for an alpha test means a wall that is 99% gone still
 * costs a draw and still casts a ragged shadow.
 *
 * @param current      The dissolve level now, 0 (solid) to `fade_target` (gone).
 * @param blocking     Whether the wall is in the way this frame.
 * @param fade_target  Dissolve level a fully-blocking wall reaches; 1 removes it entirely.
 * @param fade_in_time Seconds for a full dissolve; <= 0 snaps.
 * @param fade_out_time Seconds for a full restore; <= 0 snaps.
 * @param dt           Frame delta in seconds.
 * @return The new dissolve level, clamped to [0, fade_target].
 */
float advance_blocker_cutoff(float current, bool blocking, float fade_target,
                             float fade_in_time, float fade_out_time, float dt);

/**
 * @brief Snaps a dissolve level to one of `steps` levels.
 *
 * This is a performance measure, not a look: `alpha_cutoff` is part of the mesh batch key and
 * is hashed into the directional/local shadow caches, so a value that changes every frame
 * re-renders this caster's cached static shadow tiles every frame of the fade. Quantizing
 * spends that cost `steps` times over the whole dissolve instead of once per frame, and a
 * noise erosion hides the steps.
 *
 * @param value Dissolve level.
 * @param steps Number of levels; <= 1 disables quantization and returns `value`.
 * @return `value` snapped to the nearest level.
 */
float quantize_cutoff(float value, int steps);

/**
 * @class ViewBlockerFade
 * @brief Dissolves the owner's CUTOUT material while it blocks the view of an XraySubject.
 */
class ViewBlockerFade : public coopa::scene::Component {
public:
    std::string type_name() const override { return "ViewBlockerFade"; }

    float fade_target   = 1.0f;   /**< Dissolve level when fully blocking; 1 removes the wall. */
    float fade_in_time  = 0.18f;  /**< Seconds for a full dissolve; <= 0 snaps. */
    float fade_out_time = 0.35f;  /**< Seconds for a full restore; <= 0 snaps. */
    /**
     * Number of dissolve levels the cutoff snaps to. 32 is enough that a noise erosion reads
     * as smooth while keeping the shadow-cache invalidations to 32 over the whole fade; set 0
     * for a continuous value, at the cost of a cache miss every frame the wall is moving.
     */
    int   cutoff_steps  = 32;

    /**
     * @brief Keep casting the solid shadow while eroding (the default).
     *
     * The wall has not gone anywhere -- it is hidden for the camera's benefit, not removed from
     * the world -- so its shadow should stay. Letting the shadow dissolve with it pops the
     * lighting of the whole scene the moment the camera swings past, which reads as a much
     * bigger glitch than the hidden wall ever was. Set false for the other reading, where the
     * object genuinely vanishes.
     *
     * Costs nothing and in fact SAVES work: the shadow pass's alpha cutoff then stays constant
     * at 0 through the whole fade, so this caster stops invalidating the directional and local
     * shadow caches every time the dissolve level moves (see `cutoff_steps`, which exists to
     * blunt exactly that churn and matters much less with this on).
     */
    bool  keep_shadow   = true;

    /** @brief Marks whether the wall is in the way this frame. Called by ViewBlockerSystem. */
    void set_blocking(bool blocking) { blocking_ = blocking; }

    /** @brief True while this wall is in the way. */
    bool is_blocking() const { return blocking_; }

    /** @brief The current dissolve level, 0 (solid) to `fade_target` (gone). */
    float cutoff() const { return cutoff_; }

    /**
     * @brief Advances the dissolve and writes it into every material slot of the owner's
     *        MeshRenderer.
     *
     * Named `advance` rather than `update` so the Behaviour walk does not also apply it --
     * the system owns the timing, exactly as the kinematic controllers do.
     *
     * @param delta_time Frame delta in seconds.
     */
    void advance(float delta_time);

private:
    bool  blocking_ = false;
    float cutoff_   = 0.0f;
    float written_  = -1.0f;   ///< Last value pushed to the material; skips redundant writes.
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_VIEW_BLOCKER_H
