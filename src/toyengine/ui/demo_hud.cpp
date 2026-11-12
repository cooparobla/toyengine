#include <toyengine/ui/demo_hud.h>

#include <algorithm>
#include <cmath>

namespace toy::ui {

void DemoHud::on_update(float delta_time) {
    if (!animate || delta_time <= 0.0f) return;
    // Each vital drifts at its own rate, so a still frame shows three different fills rather
    // than three identical ones -- which is what makes a bar read as a bar.
    auto drift = [](float& v, float max, float rate, double t, float phase) {
        v = max * (0.55f + 0.45f * static_cast<float>(std::sin(t * rate + phase)));
        v = std::clamp(v, 0.0f, max);
    };
    clock_ += delta_time;
    drift(health, health_max, 0.35, clock_, 0.0f);
    drift(stamina, stamina_max, 0.90, clock_, 1.3f);
    drift(mana, mana_max, 0.20, clock_, 2.6f);
}

void DemoHud::draw(coopa::ui::imm::Context& ctx) {
    using coopa::ui::imm::Anchor;
    using coopa::ui::imm::Box;
    const GameTheme& t = theme();

    // --- Vitals, top-left. One tunable box; the three bars divide it. ---
    const Box vitals = tune().box(ctx, "vitals", Anchor::TopLeft, {24.0f, 24.0f}, {300.0f, 76.0f});
    const float gap = ctx.style.spacing;
    const float h0 = 26.0f;
    const float h1 = (vitals.h - h0 - gap * 2.0f) * 0.5f;
    ctx.stat_bar("hp", {vitals.x, vitals.y, vitals.w, h0}, "Health", health, health_max,
                 bar_style(t, t.bars.health));
    coopa::ui::imm::BarStyle minor = bar_style(t, t.bars.stamina);
    minor.show_value = false;   // the small bars are read by length, not by number
    ctx.stat_bar("sta", {vitals.x, vitals.y + h0 + gap, vitals.w, h1}, {}, stamina, stamina_max,
                 minor);
    minor = bar_style(t, t.bars.mana);
    minor.show_value = false;
    ctx.stat_bar("mana", {vitals.x, vitals.y + h0 + h1 + gap * 2.0f, vitals.w, h1}, {}, mana,
                 mana_max, minor);

    // --- Quest, top-right, right-aligned against its own box. ---
    if (!quest_title.empty()) {
        const Box q = tune().box(ctx, "quest", Anchor::TopRight, {-24.0f, 24.0f}, {360.0f, 54.0f});
        const float tw = ctx.text.width(quest_title, ctx.style.font_size * 1.2f);
        ctx.draw_text({q.right() - tw, q.y}, quest_title, ctx.style.accent,
                      ctx.style.font_size * 1.2f);
        if (!quest_step.empty()) {
            const float sw = ctx.text.width(quest_step, ctx.style.font_size);
            ctx.draw_text({q.right() - sw, q.y + ctx.text.line_height(ctx.style.font_size * 1.2f)},
                          quest_step, ctx.style.text_dim);
        }
    }

    // --- Message log, bottom-left, above the hotbar. ---
    const Box log = tune().box(ctx, "log", Anchor::BottomLeft, {24.0f, -132.0f}, {440.0f, 110.0f});
    ctx.message_log(log, messages, nullptr, false);

    // --- Hotbar, bottom-centre. ---
    const Box bar = tune().box(ctx, "hotbar", Anchor::BottomCenter, {0.0f, -56.0f},
                               {static_cast<float>(hotbar_slots) * 62.0f, 58.0f});
    std::vector<std::string> keys;
    for (int i = 0; i < hotbar_slots; ++i) keys.push_back(std::to_string((i + 1) % 10));
    coopa::ui::imm::SlotStyle slots = slot_style(t);
    ctx.hotbar("hotbar", bar, hotbar_slots, keys,
               [&](coopa::ui::imm::Context& c, const coopa::ui::imm::SlotInfo& s) {
                   if (s.pressed) hotbar_selected = s.index;
                   if (s.index == hotbar_selected) {
                       c.outline_rounded(s.box, t.slots.selected, -1.0f, 2.0f);
                   }
               },
               slots);

    // --- Key hints, along the bottom. ---
    if (!prompts.empty()) {
        ctx.prompt_bar(tune().box(ctx, "prompts", Anchor::BottomCenter, {0.0f, -12.0f},
                                  {640.0f, 34.0f}),
                       prompts);
    }
}

}  // namespace toy::ui
