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
    void draw(imm::Context& ctx, const imm::Box& box, const std::vector<std::string>& lines, float line_h = 16.0f);

private:
    int scroll_ = 0;
    bool follow_ = true;
};

} // namespace toy::editor

#endif // TOYEDITOR_UI_LOG_VIEW_H
