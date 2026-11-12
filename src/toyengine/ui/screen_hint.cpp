#include <toyengine/ui/screen_hint.h>

#include <algorithm>

namespace toy::ui {

void ScreenHint::draw(coopa::ui::imm::Context& ctx) {
    using coopa::ui::imm::Box;
    if (title.empty() && lines.empty()) return;

    // Sized to its content rather than authored: a hint that is one sentence today and three
    // tomorrow should not need its box re-measured by hand.
    const float pad = ctx.style.padding;
    const float lh = ctx.text.line_height(ctx.style.font_size);
    const float title_h = title.empty() ? 0.0f : ctx.text.line_height(ctx.style.font_size * 1.2f);
    float w = title.empty() ? 0.0f : ctx.text.width(title, ctx.style.font_size * 1.2f);
    for (const std::string& l : lines) w = std::max(w, ctx.text.width(l, ctx.style.font_size));

    // Through tune(), like every other screen, so a hint can be repositioned from
    // ui/ScreenHint.tune.yaml and shows up in the editor's Placement tab. The CONTENT size is
    // still the code's -- a hint that is one sentence today and three tomorrow re-measures
    // itself -- so only anchor and pos are taken from the file.
    const glm::vec2 content{w + pad * 2.0f,
                            title_h + lh * static_cast<float>(lines.size()) + pad * 2.0f};
    const Box placed = tune().box(ctx, "hint", anchor, offset, content);
    const Box b{placed.x, placed.y, content.x, content.y};
    if (backdrop) ctx.fill_rounded(b, ctx.style.panel_bg);

    float y = b.y + pad;
    if (!title.empty()) {
        ctx.draw_text({b.x + pad, y}, title, ctx.style.accent, ctx.style.font_size * 1.2f);
        y += title_h;
    }
    for (const std::string& l : lines) {
        ctx.draw_text({b.x + pad, y}, l, ctx.style.text_dim);
        y += lh;
    }
}

}  // namespace toy::ui
