/**
 * @file snow_system.h
 * @brief Snow trails: the system that presses things into the lying snow (SnowField) and the
 *        component that marks what presses.
 *
 *  - `SnowDeformer` on an object presses a disc into the snow under it while it is in the snow
 *    (its base no higher than the snow surface): footprints, wheel tracks, a sled.
 *    @code
 *    - type: SnowDeformer
 *      radius: 0.35     # m
 *      depth: -1        # m pressed in; < 0: down to the object's base (its lowest point)
 *      falloff: 0.5     # 0 hard-edged .. 1 a soft bowl
 *    @endcode
 *  - With the weather's `snow_auto_deformers: true`, every Rigidbody (kinematic controllers
 *    included) presses its footprint in too, without a component.
 *
 * The SnowSystem (order 370, after transforms resolve) keeps the field window on what the main
 * camera looks at (where its view ray meets the ground, kept within the window of the camera
 * itself) -- so zooming an orbit camera out does not scroll the tracks under its target out of the
 * window and clear them. It stamps every deformer, and refills trenches: back to level over the
 * weather's snow_trench_recover_time, plus faster while it snows (at the rate the cover builds). Engine::sync_surface_state_() hands the field to the renderer; the `snow` surface
 * shader (and nothing else) draws the trenches. Gameplay reads the same snow through
 * depth_at(): how deep the visible snow is at a point.
 *
 * A deformer is "in the snow" when its base is below the snow top: the ground under it (the
 * weather's precipitation map -- its sky layer, which looks through moving bodies -- lowest of
 * the 3x3 cells around, or the weather's ground_height) plus the deep-snow depth there.
 */

#ifndef TOYENGINE_WORLD_SNOW_SYSTEM_H
#define TOYENGINE_WORLD_SNOW_SYSTEM_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <fkYAML/node.hpp>

#include <toyengine/world/snow_field.h>

namespace toy {
namespace world {

/** @brief Presses a disc into the lying snow under its object. See the file doc. */
class SnowDeformer : public coopa::scene::Component {
public:
    float radius = 0.35f;   ///< m.
    float depth = -1.0f;    ///< m pressed in; < 0: down to the object's base.
    float falloff = 0.5f;   ///< 0 hard-edged .. 1 a soft bowl.
    std::string type_name() const override { return "SnowDeformer"; }
};

/** @brief Registers "SnowDeformer". */
void register_snow_components();

inline constexpr int k_snow_system_order = 370;

class SnowSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "Snow"; }
    /// Runs in edit mode only to keep the window on the camera; nothing presses in until the
    /// scene simulates.
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    const SnowField& field() const { return field_; }
    SnowField& field() { return field_; }

    /** @brief Seconds between trench refill steps (each re-uploads the field). */
    static constexpr float kRefillStep = 0.05f;

    /**
     * @brief Where the trench window centres: the ground point the camera looks at (its view ray
     *        meeting the plane z = `ground_z`), kept within `max_offset` metres of the camera's
     *        own xy; the camera's xy when it looks level or up.
     */
    static glm::vec2 focus_point(const glm::vec3& cam_pos, const glm::vec3& forward, float ground_z, float max_offset);

    /**
     * @brief How deep the visible deep snow is at world xy (m): what a `snow`-shaded surface at
     *        ground height `ground_z` is raised by there, trenches included.
     */
    float depth_at(const glm::vec2& xy, float ground_z) const;
    /** @brief The trench pressed in at world xy (m). */
    float trench_at(const glm::vec2& xy) const { return field_.trench_at(xy); }

private:
    /** @brief The ground under xy: the lowest cell of the precipitation map's sky layer (moving
     *         bodies looked through) in the 3x3 around it. */
    float ground_at_(const glm::vec2& xy) const;

    /** @brief Stamps `obj`'s footprint if its base is in the snow. radius < 0: from its bounds. */
    void press_(coopa::scene::SceneObject& obj, float radius, float depth, float falloff);

    SnowField field_;
    float cover_ = 0.0f;
    float max_depth_ = 0.0f;
    float fallback_ground_ = 0.0f;
    std::shared_ptr<const particles::GroundField> occlusion_;
    SnowStyle style_;
    float refill_pending_ = 0.0f;   ///< Metres of refill not yet applied (see kRefillStep).
    float refill_clock_ = 0.0f;
    bool had_trenches_ = false;
};

/** @brief `scene`'s snow system, or null. */
inline SnowSystem* find_snow(const coopa::scene::Scene& scene) {
    return dynamic_cast<SnowSystem*>(scene.find_system("Snow"));
}

SnowSystem* install_snow_system(coopa::scene::Scene& scene, int order = k_snow_system_order);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_SNOW_SYSTEM_H
