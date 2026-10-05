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
    RectHandle hit(const ui::UiView& v, const RectGizmoTarget& t, glm::vec2 mouse) const {
        const float hs = kHandle * 0.5f + 2.0f;
        auto near = [&](glm::vec2 c, float r) { return glm::distance(v.to_editor(c), mouse) <= r; };
        auto near_box = [&](glm::vec2 c) { const glm::vec2 e = v.to_editor(c); return std::abs(e.x - mouse.x) <= hs && std::abs(e.y - mouse.y) <= hs; };
        const ui::Rect& r = t.rect;
        if (near(pivot_point_(t), kHandle * 0.6f)) return RectHandle::Pivot;
        // Anchors (drawn outside the rect corners when they coincide with them, so the corner
        // handles stay reachable): only when they don't sit right on a resize handle.
        const glm::vec2 amin = t.parent.min + t.parent.size() * t.params.anchor_min;
        const glm::vec2 amax = t.parent.min + t.parent.size() * t.params.anchor_max;
        const std::pair<RectHandle, glm::vec2> anchors[] = {
            {RectHandle::AnchorBL, {amin.x, amin.y}}, {RectHandle::AnchorBR, {amax.x, amin.y}},
            {RectHandle::AnchorTL, {amin.x, amax.y}}, {RectHandle::AnchorTR, {amax.x, amax.y}}};
        for (const auto& [h, p] : anchors) if (near(p, kAnchor + 1.0f) && !near_box(r.min) && !near_box(r.max) &&
                                                !near_box({r.min.x, r.max.y}) && !near_box({r.max.x, r.min.y})) return h;
        const std::pair<RectHandle, glm::vec2> corners[] = {
            {RectHandle::BottomLeft, r.min}, {RectHandle::BottomRight, {r.max.x, r.min.y}},
            {RectHandle::TopLeft, {r.min.x, r.max.y}}, {RectHandle::TopRight, r.max}};
        for (const auto& [h, p] : corners) if (near_box(p)) return h;
        const glm::vec2 c = r.center();
        const std::pair<RectHandle, glm::vec2> edges[] = {
            {RectHandle::Left, {r.min.x, c.y}}, {RectHandle::Right, {r.max.x, c.y}},
            {RectHandle::Bottom, {c.x, r.min.y}}, {RectHandle::Top, {c.x, r.max.y}}};
        for (const auto& [h, p] : edges) if (near_box(p)) return h;
        const auto box = v.box_of(r);
        const bool inside = mouse.x >= box[0] && mouse.x <= box[0] + box[2] && mouse.y >= box[1] && mouse.y <= box[1] + box[3];
        if (inside) return RectHandle::Body;
        for (const auto& [h, p] : corners) {
            (void)h;
            const float d = glm::distance(v.to_editor(p), mouse);
            if (d > hs && d < hs + kRotateBand) return RectHandle::Rotate;
        }
        return RectHandle::None;
    }

    RectGizmoResult update(const ui::UiView& v, const RectGizmoTarget& t, const RectGizmoInput& in,
                           const RectGizmoSettings& s) {
        RectGizmoResult out;
        out.params = t.params;
        out.rotation = t.rotation;
        out.rect = t.rect;
        hot_ = active_ == RectHandle::None ? (in.hover_ok ? hit(v, t, in.mouse) : RectHandle::None) : active_;
        if (active_ == RectHandle::None && in.pressed && hot_ != RectHandle::None) {
            active_ = hot_;
            start_mouse_ = in.mouse;
            start_ = t;
            moved_ = false;
            out.started = true;
        }
        if (active_ == RectHandle::None) return out;
        out.handle = active_;
        out.active = true;
        if (glm::distance(in.mouse, start_mouse_) > 2.0f) moved_ = true;
        const glm::vec2 drag = v.delta_to_canvas(in.mouse - start_mouse_);
        const bool snap = s.snap != in.ctrl;
        const float threshold = s.snap_px / std::max(1e-3f, v.scale);
        ui::RectParams p = start_.params;
        float rot = start_.rotation;
        const ui::Rect& par = start_.parent;
        auto grid = [&](float x) { return snap ? ui::snap_to_grid(x, s.grid) : x; };

        switch (active_) {
            case RectHandle::Body: {
                if (start_.in_layout) { out.reorder = moved_; break; }
                ui::Rect r = start_.rect;
                r.min += drag; r.max += drag;
                glm::vec2 d(0.0f);
                if (snap) {
                    static const bool all[2][3] = {{true, true, true}, {true, true, true}};
                    d = ui::snap_rect(r, s.lines, threshold, all, &out.guides);
                }
                glm::vec2 total = drag + d;
                if (snap && d.x == 0.0f) total.x = grid(start_.rect.min.x + total.x) - start_.rect.min.x;
                if (snap && d.y == 0.0f) total.y = grid(start_.rect.min.y + total.y) - start_.rect.min.y;
                ui::move_by(p, total);
                break;
            }
            case RectHandle::Left: case RectHandle::Right: case RectHandle::Bottom: case RectHandle::Top:
            case RectHandle::BottomLeft: case RectHandle::BottomRight: case RectHandle::TopLeft: case RectHandle::TopRight: {
                int hx = 0, hy = 0;
                handle_dirs_(active_, hx, hy);
                ui::Rect r = ui::resize_rect(start_.rect, hx, hy, drag, in.alt, in.shift);
                if (snap) {
                    bool feats[2][3] = {{hx < 0, false, hx > 0}, {hy < 0, false, hy > 0}};
                    const glm::vec2 d = ui::snap_rect(r, s.lines, threshold, feats, &out.guides);
                    if (hx < 0) r.min.x += d.x; else if (hx > 0) r.max.x += d.x;
                    if (hy < 0) r.min.y += d.y; else if (hy > 0) r.max.y += d.y;
                    if (d.x == 0.0f) { if (hx < 0) r.min.x = grid(r.min.x); else if (hx > 0) r.max.x = grid(r.max.x); }
                    if (d.y == 0.0f) { if (hy < 0) r.min.y = grid(r.min.y); else if (hy > 0) r.max.y = grid(r.max.y); }
                }
                ui::set_rect(par, p, r);
                break;
            }
            case RectHandle::AnchorBL: case RectHandle::AnchorBR: case RectHandle::AnchorTL: case RectHandle::AnchorTR: {
                const glm::vec2 cm = v.to_canvas(in.mouse);
                glm::vec2 n = par.size().x > 0 && par.size().y > 0 ? (cm - par.min) / par.size() : glm::vec2(0.0f);
                n = glm::clamp(n, glm::vec2(0.0f), glm::vec2(1.0f));
                for (int a = 0; a < 2; ++a) {
                    if (snap) {
                        for (float k : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                            if (std::abs(n[a] - k) * par.size()[a] < threshold) n[a] = k;
                        }
                    }
                }
                glm::vec2 amin = start_.params.anchor_min, amax = start_.params.anchor_max;
                const bool together = glm::all(glm::lessThan(glm::abs(amin - amax), glm::vec2(1e-5f))) && !in.alt;
                if (together) { amin = amax = n; }
                else {
                    const bool left = active_ == RectHandle::AnchorBL || active_ == RectHandle::AnchorTL;
                    const bool bottom = active_ == RectHandle::AnchorBL || active_ == RectHandle::AnchorBR;
                    (left ? amin.x : amax.x) = n.x;
                    (bottom ? amin.y : amax.y) = n.y;
                    if (amin.x > amax.x) std::swap(amin.x, amax.x);
                    if (amin.y > amax.y) std::swap(amin.y, amax.y);
                }
                ui::set_anchors_keep_rect(par, p, amin, amax);
                break;
            }
            case RectHandle::Pivot: {
                const glm::vec2 cm = v.to_canvas(in.mouse);
                const ui::Rect& r = start_.rect;
                glm::vec2 pv = r.size().x > 0 && r.size().y > 0 ? (cm - r.min) / r.size() : glm::vec2(0.5f);
                if (snap) for (int a = 0; a < 2; ++a) for (float k : {0.0f, 0.5f, 1.0f}) {
                    if (std::abs(pv[a] - k) * r.size()[a] < threshold) pv[a] = k;
                }
                ui::set_pivot_keep_rect(par, p, pv);
                break;
            }
            case RectHandle::Rotate: {
                const glm::vec2 c = v.to_editor(pivot_point_(start_));
                const float a0 = std::atan2(start_mouse_.y - c.y, start_mouse_.x - c.x);
                const float a1 = std::atan2(in.mouse.y - c.y, in.mouse.x - c.x);
                // Editor Y is down, canvas Y up: a clockwise drag on screen is negative degrees.
                rot = start_.rotation - glm::degrees(a1 - a0);
                if (in.shift) rot = std::round(rot / 15.0f) * 15.0f;
                break;
            }
            default: break;
        }
        out.params = p;
        out.rotation = rot;
        out.rect = coopa::ui::resolve_rect(par, p);
        out.changed = moved_ && !out.reorder && !params_equal_(p, t.params, rot, t.rotation);
        if (in.released || !in.down) {
            out.finished = true;
            out.active = false;
            active_ = RectHandle::None;
        }
        return out;
    }

    /** @brief Draws the gizmo for `t` (call after the preview's overlays). */
    void draw(imm::Context& ctx, const ui::UiView& v, const RectGizmoTarget& t, const glm::vec4& accent) const {
        const ui::Rect& r = t.rect;
        const auto b = v.box_of(r);
        const imm::Box box{b[0], b[1], b[2], b[3]};
        ctx.outline(box, accent);
        const glm::vec4 fill{0.95f, 0.95f, 0.95f, 1.0f};
        auto handle = [&](glm::vec2 c, RectHandle h) {
            const glm::vec2 e = v.to_editor(c);
            const bool hot = hot_ == h || active_ == h;
            const float sz = hot ? kHandle + 2.0f : kHandle;
            ctx.fill({e.x - sz * 0.5f, e.y - sz * 0.5f, sz, sz}, hot ? accent : fill);
            ctx.outline({e.x - sz * 0.5f, e.y - sz * 0.5f, sz, sz}, accent);
        };
        const glm::vec2 c = r.center();
        if (!t.in_layout) {
            handle(r.min, RectHandle::BottomLeft); handle({r.max.x, r.min.y}, RectHandle::BottomRight);
            handle({r.min.x, r.max.y}, RectHandle::TopLeft); handle(r.max, RectHandle::TopRight);
        }
        handle({r.min.x, c.y}, RectHandle::Left); handle({r.max.x, c.y}, RectHandle::Right);
        handle({c.x, r.min.y}, RectHandle::Bottom); handle({c.x, r.max.y}, RectHandle::Top);
        // Anchors: four small triangles pointing at their anchor points.
        const glm::vec2 amin = t.parent.min + t.parent.size() * t.params.anchor_min;
        const glm::vec2 amax = t.parent.min + t.parent.size() * t.params.anchor_max;
        const glm::vec4 anchor_col{1.0f, 1.0f, 1.0f, 0.95f};
        auto anchor = [&](glm::vec2 p, glm::vec2 dir, RectHandle h) {
            const glm::vec2 e = v.to_editor(p);
            const bool hot = hot_ == h || active_ == h;
            const float k = hot ? kAnchor + 2.0f : kAnchor;
            const glm::vec2 d{dir.x, -dir.y};   // canvas -> editor
            const glm::vec2 n{-d.y, d.x};
            ctx.triangle(e, e + d * k * 1.6f + n * k * 0.8f, e + d * k * 1.6f - n * k * 0.8f, hot ? accent : anchor_col);
        };
        anchor({amin.x, amin.y}, {-0.7f, -0.7f}, RectHandle::AnchorBL);
        anchor({amax.x, amin.y}, {0.7f, -0.7f}, RectHandle::AnchorBR);
        anchor({amin.x, amax.y}, {-0.7f, 0.7f}, RectHandle::AnchorTL);
        anchor({amax.x, amax.y}, {0.7f, 0.7f}, RectHandle::AnchorTR);
        // Pivot.
        const glm::vec2 pv = v.to_editor(pivot_point_(t));
        const bool hot = hot_ == RectHandle::Pivot || active_ == RectHandle::Pivot;
        ctx.ring(pv, kHandle * 0.55f, 2.0f, hot ? accent : glm::vec4(0.35f, 0.65f, 1.0f, 1.0f));
    }

    RectHandle hot() const { return hot_; }
    RectHandle active() const { return active_; }
    bool dragging() const { return active_ != RectHandle::None; }
    void cancel() { active_ = RectHandle::None; }

    /** @brief The mouse cursor a handle wants (resize arrows, a hand for move). */
    static coopa::input::CursorShape cursor_for(RectHandle h) {
        using C = coopa::input::CursorShape;
        switch (h) {
            case RectHandle::Left: case RectHandle::Right: return C::ResizeH;
            case RectHandle::Top: case RectHandle::Bottom: return C::ResizeV;
            case RectHandle::Body: case RectHandle::Pivot: case RectHandle::Rotate:
            case RectHandle::AnchorBL: case RectHandle::AnchorBR: case RectHandle::AnchorTL: case RectHandle::AnchorTR: return C::Hand;
            default: return C::Arrow;
        }
    }

    static constexpr float kHandle = 8.0f;      ///< Resize handle size, editor px.
    static constexpr float kAnchor = 6.0f;      ///< Anchor triangle size.
    static constexpr float kRotateBand = 16.0f; ///< Rotation zone beyond a corner handle.

private:
    static glm::vec2 pivot_point_(const RectGizmoTarget& t) { return t.rect.min + t.rect.size() * t.params.pivot; }

    static void handle_dirs_(RectHandle h, int& hx, int& hy) {
        hx = (h == RectHandle::Left || h == RectHandle::BottomLeft || h == RectHandle::TopLeft) ? -1
           : (h == RectHandle::Right || h == RectHandle::BottomRight || h == RectHandle::TopRight) ? 1 : 0;
        hy = (h == RectHandle::Bottom || h == RectHandle::BottomLeft || h == RectHandle::BottomRight) ? -1
           : (h == RectHandle::Top || h == RectHandle::TopLeft || h == RectHandle::TopRight) ? 1 : 0;
    }

    static bool params_equal_(const ui::RectParams& a, const ui::RectParams& b, float ra, float rb) {
        auto eq = [](glm::vec2 x, glm::vec2 y) { return glm::all(glm::lessThan(glm::abs(x - y), glm::vec2(1e-5f))); };
        return eq(a.anchor_min, b.anchor_min) && eq(a.anchor_max, b.anchor_max) && eq(a.pivot, b.pivot) &&
               eq(a.anchored_position, b.anchored_position) && eq(a.size_delta, b.size_delta) && std::abs(ra - rb) < 1e-4f;
    }

    RectHandle hot_ = RectHandle::None, active_ = RectHandle::None;
    glm::vec2 start_mouse_{0.0f};
    RectGizmoTarget start_;
    bool moved_ = false;
};

} // namespace toy::editor

#endif // TOYEDITOR_VIEWPORT_RECT_GIZMO_H
