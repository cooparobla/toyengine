/**
 * @file imm_game.h
 * @brief The bodies of Context's game composites: stat bars, slot grids, hotbars, message
 *        logs, menus, prompt strips and the plates that float in the world.
 *
 * imm.h declares them and this defines them, the same split imm_icons.h uses for draw_icon()
 * -- so the core header stays about the core while a game still calls everything as
 * `ctx.stat_bar(...)`. The style structs (BarStyle, SlotStyle) live in imm.h because a default
 * argument needs them complete; they are plain colour and metric data with no game model in
 * them, which is what keeps this layer independent of coopa::stat and coopa::item.
 *
 * @code
 * const Box b = ctx.anchor(imm::Anchor::TopLeft, {20, 20}, {260, 28});
 * ctx.stat_bar("hp", b, "Health", player.health, player.max_health);
 * @endcode
 *
 * Only two composites keep state between frames -- a stat bar's damage trail and a slot's
 * cooldown, both in Context::persistent() under the caller's id. Everything else is a pure
 * function of what it was handed, which is the point of declaring a UI per frame.
 */

#ifndef UICOOPA_IMMEDIATE_IMM_GAME_H
#define UICOOPA_IMMEDIATE_IMM_GAME_H

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coopa {
namespace ui {
namespace imm {

/** @brief Fills a BarStyle's unset colours and metrics from `ctx.style`. */
inline BarStyle resolve_bar_style(const Context& ctx, BarStyle s) {
    if (s.fill.r < 0.0f)   s.fill  = ctx.style.accent;
    if (s.track.r < 0.0f)  s.track = ctx.style.field;
    if (s.trail.r < 0.0f)  s.trail = with_alpha(ctx.style.error, 0.55f);
    if (s.label.r < 0.0f)  s.label = ctx.style.text;
    if (s.rounding < 0.0f) s.rounding = ctx.style.rounding;
    return s;
}

/** @brief Fills a SlotStyle's unset colours and metrics from `ctx.style`. */
inline SlotStyle resolve_slot_style(const Context& ctx, SlotStyle s) {
    if (s.cell.r < 0.0f)       s.cell = ctx.style.field;
    if (s.cell_hover.r < 0.0f) s.cell_hover = ctx.style.field_hover;
    if (s.border.r < 0.0f)     s.border = ctx.style.border;
    if (s.spacing < 0.0f)      s.spacing = ctx.style.spacing;
    if (s.rounding < 0.0f)     s.rounding = ctx.style.rounding;
    return s;
}

// =====================================================================================
// Bars
// =====================================================================================

/**
 * @brief Fill width for a bar at fraction `f`.
 *
 * A sliver at the low end so "almost empty" still reads, clamped to the box so a bar narrower
 * than half its height cannot paint outside itself.
 */
inline float bar_fill_width(const Box& b, float f) {
    return std::min(b.w, std::max(b.h * 0.5f, b.w * f));
}

inline void Context::value_bar(const Box& b, float t, const BarStyle& style_in) {
    const BarStyle s = resolve_bar_style(*this, style_in);
    const float f = std::clamp(t, 0.0f, 1.0f);
    fill_rounded(b, s.track, s.rounding);
    if (f <= 0.0f) return;
    // A sliver still reads as "almost empty" rather than vanishing, and only the left corners
    // round until the bar is nearly full -- a rounded right edge on a half-full bar reads as
    // the bar being shorter than it is.
    fill_rounded({b.x, b.y, bar_fill_width(b, f), b.h}, s.fill, s.rounding, f > 0.98f ? kAll : kLeft);
}

inline void Context::stat_bar(std::string_view id, const Box& b, std::string_view label,
                              float value, float max, const BarStyle& style_in) {
    const BarStyle s = resolve_bar_style(*this, style_in);
    const float t = max > 0.0f ? std::clamp(value / max, 0.0f, 1.0f) : 0.0f;

    push_id(id);
    float& trail = persistent("trail", 0);
    float& hold = persistent("trail", 1);
    float& seen = persistent("trail", 2);
    float& last = persistent("trail", 3);
    pop_id();
    // First frame: start the trail level with the value, so a bar does not animate in from
    // empty the first time it is drawn.
    if (seen == 0.0f) {
        seen = 1.0f;
        trail = t;
        last = t;
    }
    const float dt = in_.dt;
    // Compared against the PREVIOUS value, not against the trail: a second hit landing while
    // the trail is still sliding has to restart the hold, or sustained damage -- the case a
    // damage trail exists for -- shows no trail at all because `hold` is already spent.
    if (t > last) {
        if (trail < t) trail = t;   // Healing: the fill passes the trail, nothing to show.
        hold = 0.0f;
    } else if (t < last) {
        hold = 0.0f;                // A new hit.
    }
    last = t;
    if (trail > t) {
        hold += dt;
        if (hold >= s.trail_hold) trail = std::max(t, trail - s.trail_rate * dt);
    } else {
        trail = t;
        hold = 0.0f;
    }

    fill_rounded(b, s.track, s.rounding);
    if (trail > t) {
        fill_rounded({b.x, b.y, bar_fill_width(b, trail), b.h}, s.trail, s.rounding,
                     trail > 0.98f ? kAll : kLeft);
    }
    if (t > 0.0f) {
        fill_rounded({b.x, b.y, bar_fill_width(b, t), b.h}, s.fill, s.rounding,
                     t > 0.98f ? kAll : kLeft);
    }

    const float pad = style.padding;
    if (!label.empty()) text_in({b.x + pad, b.y, b.w - pad * 2.0f, b.h}, label, s.label, 0.0f);
    if (s.show_value) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%lld / %lld", static_cast<long long>(std::llround(value)),
                      static_cast<long long>(std::llround(max)));
        const float w = text.width(buf, style.font_size);
        draw_text({b.right() - pad - w, b.y + (b.h - text.line_height(style.font_size)) * 0.5f},
                  buf, s.label);
    }
}

inline void Context::cooldown(const Box& b, float t) {
    const float f = std::clamp(t, 0.0f, 1.0f);
    if (f <= 0.0f) return;
    // Wipes downward, so the slot "fills back up" as it becomes usable again.
    fill({b.x, b.y, b.w, b.h * f}, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
}

// =====================================================================================
// Slots: inventories, hotbars, equipment
// =====================================================================================

template <class SlotFn>
inline void Context::slot_grid(std::string_view id, const Box& b, int cols, int rows, SlotFn&& fn,
                               const SlotStyle& style_in) {
    const SlotStyle s = resolve_slot_style(*this, style_in);
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    const float cw = (b.w - s.spacing * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const float ch = (b.h - s.spacing * static_cast<float>(rows - 1)) / static_cast<float>(rows);
    if (cw <= 0.0f || ch <= 0.0f) return;

    push_id(id);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            SlotInfo info;
            info.index = r * cols + c;
            info.box = Box{b.x + static_cast<float>(c) * (cw + s.spacing),
                           b.y + static_cast<float>(r) * (ch + s.spacing), cw, ch};
            push_id(static_cast<int64_t>(info.index));
            info.pressed = invisible_button("slot", info.box, &info.hovered, &info.held);
            // Right-click is the other half of an inventory's vocabulary (use, equip, split),
            // and a destination has to light up while something is dragged over it.
            info.right_pressed = info.hovered && mouse_pressed(Mouse::Right);
            info.drop_hovered = drag_.active && info.hovered;
            fill_rounded(info.box, info.hovered ? s.cell_hover : s.cell, s.rounding);
            outline_rounded(info.box, s.border, s.rounding);
            fn(*this, info);
            pop_id();
        }
    }
    pop_id();
}

template <class SlotFn>
inline void Context::hotbar(std::string_view id, const Box& b, int count,
                            const std::vector<std::string>& keys, SlotFn&& fn,
                            const SlotStyle& style_in) {
    slot_grid(id, b, count, 1, [&](Context& c, const SlotInfo& info) {
        fn(c, info);
        if (static_cast<size_t>(info.index) >= keys.size()) return;
        const std::string& k = keys[static_cast<size_t>(info.index)];
        if (k.empty()) return;
        const float w = c.text.width(k, c.style.font_size);
        // Bottom-right, inside the cell: the corner least likely to sit under an icon.
        c.draw_text({info.box.right() - w - 3.0f,
                     info.box.bottom() - c.text.line_height(c.style.font_size) - 1.0f},
                    k, c.style.text_dim);
    }, style_in);
}

// =====================================================================================
// Text panels
// =====================================================================================

inline void Context::message_log(const Box& b, const std::vector<std::string>& lines,
                                 const std::vector<glm::vec4>* colors, bool background) {
    if (background) fill_rounded(b, with_alpha(style.panel_bg, style.panel_bg.a * 0.8f));
    const float lh = std::max(1.0f, text.line_height(style.font_size));
    const float pad = style.padding;
    const int fits = static_cast<int>((b.h - pad * 2.0f) / lh);
    if (fits <= 0 || lines.empty()) return;

    const int count = std::min(fits, static_cast<int>(lines.size()));
    const int first = static_cast<int>(lines.size()) - count;
    push_clip(b);
    for (int i = 0; i < count; ++i) {
        const size_t src = static_cast<size_t>(first + i);
        const glm::vec4 c = (colors && src < colors->size()) ? (*colors)[src] : style.text;
        // Measured UP from the bottom edge, so the newest line is always the bottom one and a
        // log that is still filling grows upward instead of sliding down the box.
        const float y = b.bottom() - pad - static_cast<float>(count - i) * lh;
        draw_text({b.x + pad, y}, lines[src], c);
    }
    pop_clip();
}

inline void Context::prompt_bar(const Box& b,
                                const std::vector<std::pair<std::string, std::string>>& prompts) {
    const float pad = style.padding;
    const float lh = text.line_height(style.font_size);
    const float y = b.y + (b.h - lh) * 0.5f;
    float x = b.x + pad;
    push_clip(b);
    for (const auto& pair : prompts) {
        const std::string& key = pair.first;
        const std::string& action = pair.second;
        const float kw = text.width(key, style.font_size);
        // The key gets a chip so it reads as a button rather than as more prose.
        const Box chip{x, b.y + (b.h - lh - 4.0f) * 0.5f, kw + pad * 2.0f, lh + 4.0f};
        fill_rounded(chip, style.button);
        draw_text({x + pad, y}, key, style.text);
        x = chip.right() + 4.0f;
        draw_text({x, y}, action, style.text_dim);
        x += text.width(action, style.font_size) + pad * 2.0f;
    }
    pop_clip();
}

// =====================================================================================
// Menus and frames
// =====================================================================================

inline Box Context::panel_frame(std::string_view id, const Box& b, std::string_view title,
                                bool* close_clicked) {
    if (close_clicked) *close_clicked = false;
    shadow(b);
    fill_rounded(b, style.panel_bg);
    outline_rounded(b, style.border);
    if (title.empty()) return b.shrink(style.padding);

    const float bar_h = style.row_height + 4.0f;
    const Box bar{b.x, b.y, b.w, bar_h};
    fill_rounded(bar, style.header, -1.0f, kTop);
    text_in({b.x + style.padding, bar.y, b.w - bar_h - style.padding, bar_h}, title, style.text, 0.0f);
    if (close_clicked) {
        const float s = bar_h - 8.0f;
        const Box x{bar.right() - s - 4.0f, bar.y + 4.0f, s, s};
        bool hov = false;
        push_id(id);
        const bool hit = invisible_button("panel_close", x, &hov);
        pop_id();
        if (hit) *close_clicked = true;
        if (hov) fill_rounded(x, style.button_hover);
        // Drawn rather than taken from the Icon set, which has no X: Trash is the nearest
        // glyph and it says "delete this", which is not what a title bar's corner does.
        const Box g = x.shrink(5.0f);
        const glm::vec4 col = hov ? style.text : style.text_dim;
        line({g.x, g.y}, {g.right(), g.bottom()}, col, 1.5f);
        line({g.right(), g.y}, {g.x, g.bottom()}, col, 1.5f);
    }
    return Box{b.x + style.padding, bar.bottom() + style.padding, b.w - style.padding * 2.0f,
               std::max(0.0f, b.h - bar_h - style.padding * 2.0f)};
}

inline int Context::menu_list(std::string_view id, const Box& b,
                              const std::vector<std::string>& items,
                              const std::vector<bool>* disabled) {
    if (items.empty()) return -1;
    int clicked = -1;
    const float gap = style.spacing;
    const float h = (b.h - gap * static_cast<float>(items.size() - 1)) /
                    static_cast<float>(items.size());
    if (h <= 0.0f) return -1;

    push_id(id);
    for (size_t i = 0; i < items.size(); ++i) {
        const Box row{b.x, b.y + static_cast<float>(i) * (h + gap), b.w, h};
        const bool off = disabled && i < disabled->size() && (*disabled)[i];
        push_id(static_cast<int64_t>(i));
        bool hov = false;
        bool held = false;
        // Tested BEFORE registering, not after: a disabled row that still calls
        // invisible_button() claims hover, takes active_id_ and fires the click cues, so it
        // behaves exactly like an enabled one everywhere except the return value.
        if (!off && invisible_button("item", row, &hov, &held)) clicked = static_cast<int>(i);
        const glm::vec4 bg = off ? style.field
                                 : (held ? style.button_active
                                         : (hov ? style.button_hover : style.button));
        fill_rounded(row, bg);
        text_in(row, items[i], off ? style.text_disabled : style.text, 0.0f, true);
        pop_id();
    }
    pop_id();
    return clicked;
}

// =====================================================================================
// Lists: the other shape an inventory takes
// =====================================================================================

inline int Context::category_list(std::string_view id, const Box& b,
                                  const std::vector<std::string>& names, int* active) {
    if (names.empty() || !active) return -1;
    int clicked = -1;
    push_id(id);
    begin_region("cats", b, true);
    for (size_t i = 0; i < names.size(); ++i) {
        push_id(static_cast<int64_t>(i));
        if (selectable(names[i], *active == static_cast<int>(i))) {
            *active = static_cast<int>(i);
            clicked = static_cast<int>(i);
        }
        pop_id();
    }
    end_region();
    pop_id();
    return clicked;
}

inline int Context::item_list(std::string_view id, const Box& b,
                              const std::vector<ItemRow>& rows, int* selected,
                              const ItemListColumns& cols) {
    int activated = -1;
    const float h = std::max(style.row_height, 1.0f);
    const float pad = style.padding;
    const float col_w = text.width("00000", style.font_size) + pad;

    push_id(id);
    begin_region("rows", b, true);
    for (size_t i = 0; i < rows.size(); ++i) {
        const ItemRow& r = rows[i];
        push_id(static_cast<int64_t>(i));
        const Box row = next_box(h);
        // Measured from THIS ROW's right edge inwards, widest column first. Deliberately not
        // from the box passed in: the rows come from next_box() inside begin_region(), whose
        // width is the box minus its padding and any scrollbar -- so offsets taken from the
        // outer box push the numbers off the right edge the moment the list scrolls.
        float right = row.w - pad;
        const float x_value = cols.value ? (right -= col_w) : 0.0f;
        const float x_weight = cols.weight ? (right -= col_w) : 0.0f;
        const float x_count = cols.count ? (right -= col_w * 0.6f) : 0.0f;
        const float name_w = std::max(0.0f, right - pad);
        bool hov = false;
        if (invisible_button("row", row, &hov)) {
            if (selected) *selected = static_cast<int>(i);
            activated = static_cast<int>(i);
        }
        const bool sel = selected && *selected == static_cast<int>(i);
        if (sel) fill_rounded(row, style.selection);
        else if (hov) fill_rounded(row, style.button_hover);

        float x = row.x + pad;
        if (r.icon != Icon::None) {
            icon(r.icon, {x, row.y + 2.0f, h - 4.0f, h - 4.0f}, r.tint);
            x += h;
        }
        // The marks go before the name so a scanning eye finds them in one column.
        if (r.equipped) {
            text_in({x, row.y, 14.0f, row.h}, "E", style.accent, 0.0f, true);
            x += 14.0f;
        }
        if (r.favourite) {
            text_in({x, row.y, 14.0f, row.h}, "*", style.warning, 0.0f, true);
            x += 14.0f;
        }
        text_in({x, row.y, std::max(0.0f, name_w - (x - row.x)), row.h}, r.label, r.tint, 0.0f, true);

        char buf[32];
        auto right_text = [&](float x_off, const char* t) {
            const float w = text.width(t, style.font_size);
            draw_text({row.x + x_off + col_w - pad - w,
                       row.y + (row.h - text.line_height(style.font_size)) * 0.5f},
                      t, style.text_dim);
        };
        if (cols.count && r.count > 1) {
            std::snprintf(buf, sizeof(buf), "%d", r.count);
            right_text(x_count, buf);
        }
        if (cols.weight) {
            std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(r.weight));
            right_text(x_weight, buf);
        }
        if (cols.value) {
            std::snprintf(buf, sizeof(buf), "%d", r.value);
            right_text(x_value, buf);
        }
        pop_id();
    }
    if (rows.empty()) label_dim("Nothing here.");
    end_region();
    pop_id();
    // Clamped after the fact rather than before: the list can shrink between frames (an item
    // was used), and a selection past the end would silently read the wrong row.
    if (selected && !rows.empty()) *selected = std::clamp(*selected, 0, static_cast<int>(rows.size()) - 1);
    return activated;
}

inline void Context::item_detail(const Box& b, std::string_view title,
                                 std::string_view description,
                                 const std::vector<std::pair<std::string, std::string>>& stats) {
    fill_rounded(b, style.panel_alt);
    const float pad = style.padding;
    const Box inner = b.shrink(pad);
    if (inner.empty()) return;
    push_clip(inner);
    float y = inner.y;
    if (!title.empty()) {
        const float th = text.line_height(style.font_size * 1.2f);
        draw_text({inner.x, y}, title, style.text, style.font_size * 1.2f);
        y += th + 2.0f;
    }
    const float lh = text.line_height(style.font_size);
    if (!description.empty()) {
        // Wrapped here rather than through paragraph(), which lays out against the flow cursor
        // -- this card places its own lines, so it needs the width it was given.
        std::string line;
        size_t at = 0;
        while (at <= description.size() && y + lh <= inner.bottom()) {
            const size_t sp = description.find(' ', at);
            const std::string word(description.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
            const std::string tryline = line.empty() ? word : line + " " + word;
            if (!line.empty() && text.width(tryline, style.font_size) > inner.w) {
                draw_text({inner.x, y}, line, style.text_dim);
                y += lh;
                line = word;
            } else {
                line = tryline;
            }
            if (sp == std::string::npos) break;
            at = sp + 1;
        }
        if (!line.empty() && y + lh <= inner.bottom()) {
            draw_text({inner.x, y}, line, style.text_dim);
            y += lh;
        }
        y += lh * 0.5f;
    }
    for (const auto& [name, value] : stats) {
        if (y + lh > inner.bottom()) break;
        draw_text({inner.x, y}, name, style.text_dim);
        const float w = text.width(value, style.font_size);
        draw_text({inner.right() - w, y}, value, style.text);
        y += lh;
    }
    pop_clip();
}

// =====================================================================================
// Direct manipulation
// =====================================================================================

inline bool Context::drag_handle(std::string_view id, const Box& b, glm::vec2* offset) {
    if (!offset) return false;
    push_id(id);
    bool hov = false;
    bool held = false;
    invisible_button("grab", b, &hov, &held);
    float& ox = persistent("grab", 0);
    float& oy = persistent("grab", 1);
    float& grabbed = persistent("grab", 2);
    pop_id();
    // The grab point is captured once and the offset derived from it, rather than summing
    // mouse_delta every frame: a sum drifts permanently if the pointer leaves the window or a
    // host zeroes the delta mid-drag, and the panel never comes back to where it was dropped.
    if (held) {
        if (grabbed == 0.0f) {
            grabbed = 1.0f;
            ox = in_.mouse.x - offset->x;
            oy = in_.mouse.y - offset->y;
        }
        *offset = glm::vec2(in_.mouse.x - ox, in_.mouse.y - oy);
    } else {
        grabbed = 0.0f;
    }
    if (hov || held) set_mouse_cursor(coopa::input::CursorShape::Hand);
    return held;
}

inline void Context::floating_text(glm::vec2 at, std::string_view s, const glm::vec4& color,
                                   float size) {
    const float px = size > 0.0f ? size : style.font_size;
    const float w = text.width(s, px);
    draw_text({at.x - w * 0.5f, at.y - text.line_height(px) * 0.5f}, s, color, px);
}

inline void Context::nameplate(std::string_view id, const Box& b, std::string_view name,
                               float value, float max, const BarStyle& style_in) {
    BarStyle s = style_in;
    s.show_value = false;   // No room, and a plate is read at a glance rather than studied.
    const float lh = text.line_height(style.font_size);
    const float bar_h = std::max(4.0f, std::min(b.h - lh - 2.0f, b.h * 0.35f));
    if (!name.empty()) {
        const float w = text.width(name, style.font_size);
        draw_text({b.x + (b.w - w) * 0.5f, b.y}, name, style.text);
    }
    stat_bar(id, {b.x, b.bottom() - bar_h, b.w, bar_h}, {}, value, max, s);
}

}  // namespace imm
}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_IMMEDIATE_IMM_GAME_H
