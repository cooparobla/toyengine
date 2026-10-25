#include "editor/ui/curve_widget.h"

#include <algorithm>
#include <cstdio>

namespace toy {
namespace editor {
namespace ui {

namespace {

constexpr float kPad = 6.0f;      ///< Border between the widget's frame and the curve's unit square.
constexpr float kHandle = 11.0f;  ///< Hit box around a point, in pixels.

CurveBox inner(const imm::Box& b) { return {b.x + kPad, b.y + kPad, b.w - 2 * kPad, b.h - 2 * kPad}; }

} // namespace

ShapeEdit curve_edit(imm::Context& ctx, std::string_view id, std::vector<glm::vec2>& points, float height) {
    ShapeEdit e;
    ctx.push_id(id);
    const imm::Box box = ctx.next_box(height);
    const CurveBox cb = inner(box);
    const glm::vec2 mouse = ctx.mouse();

    // Hit areas first, points before the background: the first widget to claim a press keeps
    // it, so registering the background first would swallow every drag.
    int dragging = -1;
    int remove = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        const glm::vec2 at = curve_to_box(cb, points[i]);
        const imm::Box hb{at.x - kHandle * 0.5f, at.y - kHandle * 0.5f, kHandle, kHandle};
        bool hovered = false, held = false;
        ctx.push_id(static_cast<int64_t>(i));
        ctx.invisible_button("point", hb, &hovered, &held);
        if (held) dragging = static_cast<int>(i);
        if (ctx.last_clicked(coopa::ui::imm::Mouse::Right)) remove = static_cast<int>(i);
        if (ctx.last_deactivated()) e.finished = true;
        ctx.pop_id();
    }
    bool bg_hovered = false, bg_held = false;
    const bool bg_clicked = ctx.invisible_button("bg", box, &bg_hovered, &bg_held);

    if (dragging >= 0 && move_curve_point(points, dragging, box_to_curve(cb, mouse))) e.changed = true;
    if (remove >= 0) {
        if (remove_curve_point(points, remove)) e.changed = e.finished = true;
    } else if (bg_clicked && dragging < 0) {
        insert_curve_point(points, box_to_curve(cb, mouse));
        e.changed = e.finished = true;
    }
    e.dragging = dragging >= 0;

    // Then the picture, in back-to-front order.
    ctx.fill_rounded(box, ctx.style.field);
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) * 0.25f;
        ctx.fill({cb.x, cb.y + cb.h * t, cb.w, 1.0f}, ctx.style.separator);
        ctx.fill({cb.x + cb.w * t, cb.y, 1.0f, cb.h}, ctx.style.separator);
    }
    for (size_t i = 1; i < points.size(); ++i) {
        ctx.line(curve_to_box(cb, points[i - 1]), curve_to_box(cb, points[i]), ctx.style.accent, 1.6f);
    }
    for (size_t i = 0; i < points.size(); ++i) {
        const glm::vec2 at = curve_to_box(cb, points[i]);
        const bool hot = dragging == static_cast<int>(i);
        ctx.circle(at, hot ? 5.0f : 4.0f, hot ? ctx.style.text : ctx.style.accent);
    }
    if (points.empty()) ctx.text_in(box, "Click to add a point", ctx.style.text_disabled, 0.0f, true);
    ctx.outline_rounded(box, ctx.style.border);
    if (bg_hovered && dragging < 0) ctx.tooltip("Curve\nDrag a point to move it, click to add one, right-click one to remove it");
    ctx.pop_id();
    return e;
}

ShapeEdit gradient_edit(imm::Context& ctx, std::string_view id, std::vector<ColorStop>& stops, float height) {
    ShapeEdit e;
    ctx.push_id(id);
    float& selection = ctx.persistent("gradient_selection");
    int selected = std::clamp(static_cast<int>(selection), 0, std::max(0, static_cast<int>(stops.size()) - 1));
    const imm::Box box = ctx.next_box(height);
    const CurveBox cb{box.x + kPad, box.y, box.w - 2 * kPad, box.h};
    const glm::vec2 mouse = ctx.mouse();

    int dragging = -1;
    int remove = -1;
    for (size_t i = 0; i < stops.size(); ++i) {
        const float x = cb.x + std::clamp(stops[i].t, 0.0f, 1.0f) * cb.w;
        const imm::Box hb{x - kHandle * 0.5f, box.y, kHandle, box.h};
        bool hovered = false, held = false;
        ctx.push_id(static_cast<int64_t>(i));
        ctx.invisible_button("stop", hb, &hovered, &held);
        if (held) { dragging = static_cast<int>(i); selected = static_cast<int>(i); }
        if (ctx.last_clicked(coopa::ui::imm::Mouse::Right)) remove = static_cast<int>(i);
        if (ctx.last_deactivated()) e.finished = true;
        ctx.pop_id();
    }
    bool bar_hovered = false, bar_held = false;
    const bool bar_clicked = ctx.invisible_button("bar", box, &bar_hovered, &bar_held);

    if (dragging >= 0) {
        const float t = cb.w > 0 ? std::clamp((mouse.x - cb.x) / cb.w, 0.0f, 1.0f) : 0.0f;
        if (t != stops[static_cast<size_t>(dragging)].t) {
            selected = move_gradient_stop(stops, dragging, t);
            dragging = selected;
            e.changed = true;
        }
    }
    if (remove >= 0) {
        if (remove_gradient_stop(stops, remove)) {
            selected = std::min(selected, static_cast<int>(stops.size()) - 1);
            e.changed = e.finished = true;
        }
    } else if (bar_clicked && dragging < 0) {
        const float t = cb.w > 0 ? std::clamp((mouse.x - cb.x) / cb.w, 0.0f, 1.0f) : 0.0f;
        selected = insert_gradient_stop(stops, t);
        e.changed = e.finished = true;
    }
    e.dragging = dragging >= 0;

    // The ramp, a column of samples wide enough to read as a smooth blend.
    ctx.fill_rounded(box, ctx.style.field);
    const int columns = std::max(1, static_cast<int>(cb.w));
    for (int i = 0; i < columns; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(columns);
        const glm::vec3 c = sample_gradient(stops, t);
        ctx.fill({cb.x + static_cast<float>(i), box.y + 2.0f, 1.0f, box.h - 8.0f}, glm::vec4(c, 1.0f));
    }
    for (size_t i = 0; i < stops.size(); ++i) {
        const float x = cb.x + std::clamp(stops[i].t, 0.0f, 1.0f) * cb.w;
        const bool on = selected == static_cast<int>(i);
        ctx.fill_rounded({x - 3.0f, box.bottom() - 7.0f, 6.0f, 6.0f}, on ? ctx.style.text : ctx.style.text_dim, 2.0f);
    }
    ctx.outline_rounded(box, ctx.style.border);
    if (bar_hovered && dragging < 0) ctx.tooltip("Gradient\nDrag a stop to move it, click the bar to add one, right-click a stop to remove it");
    selection = static_cast<float>(selected);
    e.selected = stops.empty() ? -1 : selected;
    ctx.pop_id();
    return e;
}

} // namespace ui
} // namespace editor
} // namespace toy
