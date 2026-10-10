/**
 * @file rect_gizmo.h
 * @brief The UI designer's rect gizmo: move, resize, anchor, pivot and rotate one UI element
 *        by dragging handles on the preview -- Unity's Rect Tool, with snapping guides.
 *
 * Pure interaction logic: it is fed the mouse (editor pixels) and the element's geometry, and
 * returns the RectParams the drag produces. The editor draws it (draw()) and writes the
 * result into the document; tests drive update() directly.
 *
 *   Body           drag to move. Inside a layout group the group owns position, so the
 *                  editor turns a body drag into a reorder instead (RectGizmoResult::reorder).
 *   8 handles      resize; the opposite edge stays put. Alt: about the centre. Shift: keep
 *                  the aspect ratio (corners).
 *   4 diamonds     the anchors, at their points in the parent. Together they move as one; drag
 *                  one alone to split them (a fixed rect becomes a stretching one). The rect
 *                  never jumps -- only how it is expressed changes.
 *   Pivot (ring)   drag to move the pivot (rotation and scale centre); the rect stays put.
 *   Outside a corner  rotate about the pivot; Shift snaps to 15 degrees.
 *
 * Snapping (on unless Ctrl inverts it): to the parent's and siblings' edges and centres within
 * a few pixels (the hit lines come back as guides), and to the pixel grid.
 */

#ifndef TOYEDITOR_VIEWPORT_RECT_GIZMO_H
#define TOYEDITOR_VIEWPORT_RECT_GIZMO_H

#include "../ui/ui_canvas_math.h"

#include <uicoopa/immediate/imm.h>

#include <cmath>
#include <vector>

namespace toy::editor {

namespace imm = coopa::ui::imm;

enum class RectHandle {
    None, Body,
    Left, Right, Bottom, Top, BottomLeft, BottomRight, TopLeft, TopRight,
    AnchorBL, AnchorBR, AnchorTL, AnchorTR,
    Pivot, Rotate,
};

/** @brief The element being edited, in canvas space (Y up). */
struct RectGizmoTarget {
    ui::Rect rect;          ///< Its resolved rect.
    ui::Rect parent;        ///< Its parent's resolved rect.
    ui::RectParams params;
    float rotation = 0.0f;  ///< Degrees.
    bool in_layout = false; ///< A layout group positions it (move becomes reorder).
};

struct RectGizmoInput {
    glm::vec2 mouse{0.0f};   ///< Editor pixels.
    bool pressed = false, down = false, released = false;
    bool shift = false, alt = false, ctrl = false;
    bool hover_ok = true;    ///< The mouse is over the preview (not a popup / other panel).
};

struct RectGizmoSettings {
    bool snap = true;          ///< Snapping on (Ctrl inverts while dragging).
    float snap_px = 6.0f;      ///< Snap distance, editor pixels.
    float grid = 1.0f;         ///< Canvas-pixel grid for positions and sizes (<= 0: none).
    std::vector<ui::SnapLine> lines;   ///< Parent / sibling edges and centres.
};

struct RectGizmoResult {
    bool started = false, active = false, finished = false;
    bool changed = false;           ///< params / rotation differ from the target's this frame.
    bool reorder = false;           ///< A body drag inside a layout group (see the file comment).
    RectHandle handle = RectHandle::None;
    ui::RectParams params;
    float rotation = 0.0f;
    ui::Rect rect;                  ///< The rect params resolve to (canvas space).
    std::vector<ui::SnapLine> guides;
};

class RectGizmo {
public:
    /** @brief Which handle is under `mouse` (editor pixels). */
    RectHandle hit(const ui::UiView& v, const RectGizmoTarget& t, glm::vec2 mouse) const;

    RectGizmoResult update(const ui::UiView& v, const RectGizmoTarget& t, const RectGizmoInput& in,
                           const RectGizmoSettings& s);

    /** @brief Draws the gizmo for `t` (call after the preview's overlays). */
    void draw(imm::Context& ctx, const ui::UiView& v, const RectGizmoTarget& t, const glm::vec4& accent) const;

    RectHandle hot() const { return hot_; }
    RectHandle active() const { return active_; }
    bool dragging() const { return active_ != RectHandle::None; }
    void cancel() { active_ = RectHandle::None; }

    /** @brief The mouse cursor a handle wants (resize arrows, a hand for move). */
    static coopa::input::CursorShape cursor_for(RectHandle h);

    static constexpr float kHandle = 8.0f;      ///< Resize handle size, editor px.
    static constexpr float kAnchor = 6.0f;      ///< Anchor triangle size.
    static constexpr float kRotateBand = 16.0f; ///< Rotation zone beyond a corner handle.

private:
    static glm::vec2 pivot_point_(const RectGizmoTarget& t) { return t.rect.min + t.rect.size() * t.params.pivot; }

    static void handle_dirs_(RectHandle h, int& hx, int& hy);

    static bool params_equal_(const ui::RectParams& a, const ui::RectParams& b, float ra, float rb);

    RectHandle hot_ = RectHandle::None, active_ = RectHandle::None;
    glm::vec2 start_mouse_{0.0f};
    RectGizmoTarget start_;
    bool moved_ = false;
};

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_RECT_GIZMO_H
