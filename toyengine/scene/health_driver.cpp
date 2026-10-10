#include <toyengine/scene/health_driver.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/stat/resource.h>
#include <uicoopa/widgets/progress_bar.h>

namespace toy {
namespace scene {

void HealthDriver::start() {
    health_.max = max_health;
    health_.current = std::clamp(start_health, 0.0f, max_health);

    bar_ = find_bar_();
    if (bar_) {
        // bind() syncs the bar to the Resource immediately AND follows its on_changed
        // signal from here on, so update() below never touches the bar again.
        bar_->bind(&health_);
    }
}

void HealthDriver::update(float delta_time) {
    // A bar a composite builds (a StatBar's) only exists once that composite has started,
    // which can be after this component's own start(): look again until it turns up.
    if (!bar_ && (bar_ = find_bar_())) bar_->bind(&health_);
    if (delta_time <= 0.0f || health_.max <= 0.0f) return;

    if (draining_) {
        health_.damage(damage_per_second * delta_time);
        if (health_.normalized() <= turnaround_fraction) draining_ = false;
    } else {
        health_.heal(regen_per_second * delta_time);
        if (health_.is_full()) draining_ = true;
    }
}

} // namespace scene
} // namespace toy
