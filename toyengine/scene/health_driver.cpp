#include <toyengine/scene/health_driver.h>

#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {

void HealthDriver::start() {
    health_.max = max_health;
    health_.current = std::clamp(start_health, 0.0f, max_health);
    // A plate above the thing it labels, facing the camera and readable through geometry.
    space = toy::ui::ImmSpace::World;
    ImmScreen::start();
}

void HealthDriver::on_update(float delta_time) {
    if (delta_time <= 0.0f || health_.max <= 0.0f) return;

    if (draining_) {
        health_.damage(damage_per_second * delta_time);
        if (health_.normalized() <= turnaround_fraction) draining_ = false;
    } else {
        health_.heal(regen_per_second * delta_time);
        if (health_.is_full()) draining_ = true;
    }
}

void HealthDriver::draw(coopa::ui::imm::Context& ctx) {
    using coopa::ui::imm::Anchor;
    using coopa::ui::imm::Box;

    const Box plate = ctx.stretch();
    ctx.fill_rounded(plate, theme().world.plate_bg);

    // The button takes the bottom strip when there is one, so the bar never sits under it.
    const float button_h = show_heal_button ? ctx.style.row_height + 4.0f : 0.0f;
    const Box body{plate.x, plate.y, plate.w, plate.h - button_h};

    coopa::ui::imm::BarStyle bar = toy::ui::bar_style(theme(), theme().bars.health);
    bar.label = theme().world.plate_text;
    ctx.nameplate("health", body.shrink(4.0f), label, health_.current, health_.max, bar);

    if (show_heal_button) {
        const Box b{plate.x + 6.0f, plate.bottom() - button_h, plate.w - 12.0f, button_h - 4.0f};
        // A clickable widget on a world canvas: the host turns the pointer into a ray, the
        // canvas turns that into a point in its own pixels, and the button never knows.
        if (ctx.button("Heal", b)) health_.heal(health_.max * 0.35f);
    }
}

}  // namespace scene
}  // namespace toy
