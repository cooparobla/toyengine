/**
 * @file health_driver.h
 * @brief Demo component that owns a health Resource and draws it as a plate in the world.
 *
 * The gameplay half of the world-space UI demo, and the shape a game's own nameplate takes:
 * one component owns the model and declares the plate that shows it. There is nothing to
 * bind and nothing to look up -- draw() reads `health_` directly, every frame, so the two can
 * never disagree and a bar cannot be left pointing at a Resource that has gone away.
 *
 * The damage/regen cycle exists so a screenshot or headless capture shows a partially drained
 * bar: a full bar is indistinguishable from a plain panel.
 *
 * @code{.yaml}
 * - type: HealthDriver
 *   label: Aldric
 *   max_health: 100.0
 *   damage_per_second: 22.0
 *   world_size: {x: 260, y: 60}
 * @endcode
 */

#ifndef TOYENGINE_SCENE_HEALTH_DRIVER_H
#define TOYENGINE_SCENE_HEALTH_DRIVER_H

#include <algorithm>
#include <string>

#include <coopa/stat/resource.h>

#include <toyengine/ui/imm_screen.h>

namespace toy {
namespace scene {

/**
 * @class HealthDriver
 * @brief Owns a Resource and shows it on a world-space plate above its object.
 */
class HealthDriver : public toy::ui::ImmScreen {
public:
    std::string type_name() const override { return "HealthDriver"; }

    /** @brief Name drawn above the bar. Empty draws the bar alone. */
    std::string label;

    float max_health        = 100.0f;
    float start_health      = 100.0f;
    float damage_per_second = 22.0f;  /**< Drain rate while draining. 0 freezes the cycle. */
    float regen_per_second  = 14.0f;  /**< Refill rate once depleted. */
    /** @brief Health below which the cycle turns around and starts refilling, as a fraction
     *         of max. Not 0: bottoming out fully makes the bar read as "broken", and the
     *         damage trail has nothing left to trail behind. */
    float turnaround_fraction = 0.15f;

    /** @brief Draw a Heal button on the plate -- the demo's clickable world-space widget. */
    bool show_heal_button = false;

    /** @brief The Resource this component owns, for code that wants to damage or heal it. */
    coopa::stat::Resource& health() { return health_; }

    void start() override;
    void on_update(float delta_time) override;
    void draw(coopa::ui::imm::Context& ctx) override;

private:
    coopa::stat::Resource health_{100.0f};
    bool draining_ = true;
};

}  // namespace scene
}  // namespace toy

#endif  // TOYENGINE_SCENE_HEALTH_DRIVER_H
