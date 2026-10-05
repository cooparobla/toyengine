/**
 * @file log_view.h
 * @brief A scrolling view of streamed output lines (build logs, tools/toyhub runs) that keeps the
 *        newest line in view -- until the user scrolls up to read something; scrolling back to the
 *        bottom follows the output again. Shared by the hub's log drawer and the editor's
 *        Build Project window.
 */

#ifndef TOYEDITOR_UI_LOG_VIEW_H
#define TOYEDITOR_UI_LOG_VIEW_H

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace toy::editor {

namespace imm = coopa::ui::imm;

class LogView {
public:
    /** @brief Follow the newest line again (e.g. when a new run starts). */
    void follow() { follow_ = true; }
    /** @brief True while the view sticks to the newest line. */
    bool following() const { return follow_; }
    /** @brief First visible line, as last drawn. */
    int scroll() const { return scroll_; }

    /**
     * @brief Draws `lines` into `box`. Error / warning lines are tinted; lines starting with "$ "
     *        (the echoed command) are dimmed.
     */
    void draw(imm::Context& ctx, const imm::Box& box, const std::vector<std::string>& lines, float line_h = 16.0f) {
        const imm::Style& st = ctx.style;
        ctx.fill(box, st.field);
        ctx.outline(box, st.border);
        const int visible = std::max(1, static_cast<int>((box.h - 8) / line_h));
        const int total = static_cast<int>(lines.size());
        const int max_scroll = std::max(0, total - visible);

        // Wheel: scrolling up leaves the tail; reaching the bottom again resumes following.
        if (ctx.is_hovered(box) && ctx.input().scroll.y != 0.0f) {
            const int step = static_cast<int>(std::round(ctx.input().scroll.y * 3.0f));
            scroll_ = std::clamp((follow_ ? max_scroll : scroll_) - (step == 0 ? (ctx.input().scroll.y > 0 ? 1 : -1) : step), 0, max_scroll);
            follow_ = scroll_ >= max_scroll;
        }
        // Scrollbar drag.
        const float track_h = box.h - 4;
        const float grab_h = max_scroll > 0 ? std::max(20.0f, track_h * visible / static_cast<float>(total)) : track_h;
        const imm::Box track{box.right() - st.scrollbar - 2, box.y + 2, st.scrollbar, track_h};
        bool hov = false, held = false;
        if (max_scroll > 0) {
            ctx.invisible_button("##logscroll", track, &hov, &held);
            if (held) {
                const float t = std::clamp((ctx.mouse().y - track.y - grab_h * 0.5f) / std::max(1.0f, track_h - grab_h), 0.0f, 1.0f);
                scroll_ = static_cast<int>(std::round(t * max_scroll));
                follow_ = scroll_ >= max_scroll;
            }
        }
        if (follow_) scroll_ = max_scroll;
        scroll_ = std::clamp(scroll_, 0, max_scroll);

        ctx.push_clip(box.shrink(1));
        for (int i = 0; i < visible && scroll_ + i < total; ++i) {
            const std::string& l = lines[static_cast<size_t>(scroll_ + i)];
            const bool cmd = l.rfind("$ ", 0) == 0;
            const bool err = !cmd && (l.find("error") != std::string::npos || l.find("Error") != std::string::npos);
            const bool warn = !cmd && !err && l.find("warning:") != std::string::npos;
            ctx.text_in({box.x + 6, box.y + 4 + i * line_h, box.w - 12 - st.scrollbar, line_h}, l,
                        err ? st.error : warn ? st.warning : cmd ? st.text_disabled : st.text_dim, 0.0f);
        }
        ctx.pop_clip();
        if (max_scroll > 0) {
            const float t = static_cast<float>(scroll_) / static_cast<float>(max_scroll);
            ctx.fill_rounded({track.x, track.y + (track_h - grab_h) * t, track.w, grab_h}, held || hov ? st.text_dim : st.scroll_grab,
                             track.w * 0.5f);
        }
    }

private:
    int scroll_ = 0;
    bool follow_ = true;
};

} // namespace toy::editor

#endif // TOYEDITOR_UI_LOG_VIEW_H
