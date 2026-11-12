#include <toyengine/ui/world_label.h>

namespace toy::ui {

void WorldLabel::start() {
    space = ImmSpace::World;
    ImmScreen::start();
}

glm::vec4 WorldLabel::bar_color(const coopa::ui::imm::Context& ctx) const {
    const GameTheme& t = theme();
    if (bar_role == "mana") return t.bars.mana;
    if (bar_role == "stamina") return t.bars.stamina;
    if (bar_role == "shield") return t.bars.shield;
    if (bar_role == "experience") return t.bars.experience;
    if (bar_role == "health") return t.bars.health;
    return ctx.style.accent;
}

void WorldLabel::draw(coopa::ui::imm::Context& ctx) {
    using coopa::ui::imm::Box;

    const Box plate = ctx.stretch();
    if (backdrop) ctx.fill_rounded(plate, theme().world.plate_bg);

    const Box inner = plate.shrink(5.0f);
    const bool has_bar = bar_max > 0.0f;
    const float bar_h = has_bar ? std::max(5.0f, inner.h * 0.22f) : 0.0f;
    float y = inner.y;

    if (!title.empty()) {
        // The title carries the plate, so it takes the larger size; everything else is
        // measured against what is left.
        const float size = ctx.style.font_size * 1.25f;
        const float w = ctx.text.width(title, size);
        ctx.draw_text({inner.x + (inner.w - w) * 0.5f, y}, title, theme().world.plate_text, size);
        y += ctx.text.line_height(size);
    }
    if (!subtitle.empty()) {
        const float w = ctx.text.width(subtitle, ctx.style.font_size);
        // Centred text wider than the plate spills off BOTH edges and is scissored mid-word.
        // A fragment is worse than nothing, so a subtitle that does not fit is not drawn.
        if (w <= inner.w) {
            ctx.draw_text({inner.x + (inner.w - w) * 0.5f, y}, subtitle, ctx.style.text_dim);
        }
    }
    if (has_bar) {
        coopa::ui::imm::BarStyle s = bar_style(theme(), bar_color(ctx));
        s.show_value = false;   // read at a glance, not studied
        ctx.stat_bar("bar", {inner.x, inner.bottom() - bar_h, inner.w, bar_h}, {},
                     bar_value, bar_max, s);
    }
}

}  // namespace toy::ui
