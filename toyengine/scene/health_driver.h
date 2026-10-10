/**
 * @file health_driver.h
 * @brief Demo component that owns a coopa::stat::Resource and binds it to a uicoopa
 *        ProgressBar in its own subtree -- the gameplay half of the world-space canvas demo.
 *
 * The point of this file is the *seam*: uicoopa's widgets are pure visualizations of a model
 * they do not own, so a health bar needs someone to own the health. That is exactly one call
 * -- ProgressBar::bind(&health_) -- after which the bar tracks the Resource's on_changed
 * signal and drives its own chip-damage trail with no further help. The Resource is a MEMBER,
 * so it necessarily outlives the binding it hands out.
 *
 * The damage/regen cycle exists so a screenshot or headless capture shows a partially drained
 * bar: a full ProgressBar is indistinguishable from a plain panel.
 */

#ifndef TOYENGINE_SCENE_HEALTH_DRIVER_H
#define TOYENGINE_SCENE_HEALTH_DRIVER_H

#include <algorithm>
#include <string>


#include <uicoopa/binding/ui_handle.h>

namespace toy {
namespace scene {

/**
 * @class HealthDriver
 * @brief Drives a descendant ProgressBar from a Resource this component owns.
 *
 * Example YAML, on the same object as (or an ancestor of) the bar:
 * @code
 * - type: HealthDriver
 *   bar_object: HealthBar        # descendant name; empty searches the whole subtree
 *   max_health: 100.0
 *   damage_per_second: 22.0
 *   regen_per_second: 14.0
 *   start_health: 100.0
 * @endcode
 */
class HealthDriver : public coopa::scene::Component {
public:
    /** @brief Name of the descendant SceneObject carrying the ProgressBar. Empty means
     *         "the first ProgressBar anywhere in this object's subtree". */
    std::string bar_object;

    float max_health        = 100.0f;
    float start_health      = 100.0f;
    float damage_per_second = 22.0f;  /**< Drain rate while draining. 0 freezes the cycle. */
    float regen_per_second  = 14.0f;  /**< Refill rate once depleted. */
    /** @brief Health below which the cycle turns around and starts refilling, as a fraction
     *         of max. Not 0: bottoming out fully makes the bar read as "broken", and the
     *         chip-damage trail has nothing left to trail behind. */
    float turnaround_fraction = 0.15f;

    std::string type_name() const override { return "HealthDriver"; }

    /** @brief The Resource this component owns, for code that wants to damage/heal directly
     *         (e.g. a world-space "Heal" Button wired up by the host). */
    coopa::stat::Resource& health() { return health_; }

    void start() override;

    void update(float delta_time) override;

private:
    /** @brief Resolves bar_object (or the first ProgressBar in the subtree) at start() time --
     *         never in a YAML parser, which runs before this object's children exist. */
    /**
     * @brief The bar this drives, by name through the same lookup game code binds authored UI
     *        with (UiHandle): `bar_object` names it -- or an object, a StatBar say, carrying
     *        one below it -- and empty means this object or the first bar in its subtree.
     */
    coopa::ui::ProgressBar* find_bar_() const {
        return coopa::ui::UiHandle(owner).find<coopa::ui::ProgressBar>(bar_object);
    }

    coopa::stat::Resource   health_{100.0f};
    coopa::ui::ProgressBar* bar_ = nullptr;
    bool draining_ = true;
};

}  // namespace scene
}  // namespace toy

#endif  // TOYENGINE_SCENE_HEALTH_DRIVER_H
