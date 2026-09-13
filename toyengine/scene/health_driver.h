/**
 * @file health_driver.h
 * @brief Demo component that owns a coopa::stat::Resource and binds it to a uicoopa
 *        ProgressBar in its own subtree -- the gameplay half of the world-space canvas demo.
 *
 * The point of this file is the *seam*, not the saw-tooth: uicoopa's widgets are deliberately
 * pure visualizations of a model they do not own (see uicoopa/widgets/inventory_binding.h's
 * ownership contract), so a health bar needs someone to own the health. That is exactly one
 * call -- ProgressBar::bind(&health_) -- after which the bar tracks the Resource's
 * on_changed signal and even drives its own chip-damage trail with no further help.
 *
 * Shaped like toyengine/scene/kinematic_mover.h: a plain coopa::scene::Component that only
 * touches its own fields and its own subtree, registered as `type: HealthDriver` in
 * toyengine/scene/register.h. The Resource is a MEMBER, so it necessarily outlives the
 * binding it hands out.
 *
 * The damage/regen cycle exists so a screenshot or a headless capture shows a partially
 * drained bar rather than a permanently full one -- a full ProgressBar is indistinguishable
 * from a plain panel, which would make the demo prove nothing.
 */

#ifndef TOYENGINE_SCENE_HEALTH_DRIVER_H
#define TOYENGINE_SCENE_HEALTH_DRIVER_H

#include <algorithm>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/stat/resource.h>

#include <uicoopa/widgets/progress_bar.h>

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

    void start() override {
        health_.max = max_health;
        health_.current = std::clamp(start_health, 0.0f, max_health);

        bar_ = find_bar_();
        if (bar_) {
            // bind() syncs the bar to the Resource immediately AND follows its on_changed
            // signal from here on, so update() below never touches the bar again.
            bar_->bind(&health_);
        }
    }

    void update(float delta_time) override {
        if (delta_time <= 0.0f || health_.max <= 0.0f) return;

        if (draining_) {
            health_.damage(damage_per_second * delta_time);
            if (health_.normalized() <= turnaround_fraction) draining_ = false;
        } else {
            health_.heal(regen_per_second * delta_time);
            if (health_.is_full()) draining_ = true;
        }
    }

private:
    /** @brief Resolves bar_object (or the first ProgressBar in the subtree) at start() time --
     *         never in a YAML parser, which runs before this object's children exist. */
    coopa::ui::ProgressBar* find_bar_() const {
        if (!owner) return nullptr;
        if (!bar_object.empty()) {
            if (auto* node = owner->find_descendant(bar_object)) {
                return node->get_component<coopa::ui::ProgressBar>();
            }
            return nullptr;
        }
        if (auto* self = owner->get_component<coopa::ui::ProgressBar>()) return self;
        return find_in_subtree_(*owner);
    }

    static coopa::ui::ProgressBar* find_in_subtree_(coopa::scene::SceneObject& obj) {
        for (auto& child : obj.children()) {
            if (auto* bar = child->get_component<coopa::ui::ProgressBar>()) return bar;
            if (auto* deeper = find_in_subtree_(*child)) return deeper;
        }
        return nullptr;
    }

    coopa::stat::Resource   health_{100.0f};
    coopa::ui::ProgressBar* bar_ = nullptr;
    bool draining_ = true;
};

}  // namespace scene
}  // namespace toy

#endif  // TOYENGINE_SCENE_HEALTH_DRIVER_H
