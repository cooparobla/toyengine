#include "editor/app/ui/tool_strip.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {
namespace ui {

imm::Box ToolStrip::next() {
    const float h = size > 0.0f ? size : bar.h;
    const float y = bar.y + (bar.h - h) * 0.5f;
    const imm::Box b = rtl ? imm::Box{bar.right() - cursor - h, y, h, h}
                           : imm::Box{bar.x + cursor, y, h, h};
    cursor += h + gap;
    return b;
}

bool ToolStrip::button(imm::Context& ctx, const char* id, imm::Icon icon, const char* tip, bool on,
                       bool enabled) {
    const imm::Box b = next();
    if (app) app->set_test_rect(rect_prefix + id, b);
    if (!enabled) {
        // Drawn rather than skipped: a strip whose buttons come and go moves every button
        // beside them, so an unavailable one holds its place and reads as unavailable.
        if (framed) ctx.fill_rounded(b, ctx.style.panel_alt);
        ctx.icon(icon, b.shrink(std::max(2.0f, b.h * 0.16f)), ctx.style.text_disabled);
        return false;
    }
    return ctx.icon_button(id, icon, tip, on, b.h, imm::Context::kAll, b, framed);
}

bool tool_header_button(imm::Context& ctx, const imm::Box& hb, float& x, const char* id, imm::Icon icon,
                        const char* tip, bool on) {
    const float bh = hb.h - 6;
    const bool clicked = ctx.icon_button(id, icon, tip, on, bh, imm::Context::kAll, imm::Box{x, hb.y + 3, bh, bh});
    x += bh + 3;
    return clicked;
}

void tool_header_label(imm::Context& ctx, const imm::Box& hb, float& x, const char* text) {
    const float w = ctx.text_width(text) + 6;
    ctx.text_in({x, hb.y, w, hb.h}, text, ctx.style.text_dim, 0.0f);
    x += w;
}

} // namespace ui
} // namespace editor
} // namespace toy
