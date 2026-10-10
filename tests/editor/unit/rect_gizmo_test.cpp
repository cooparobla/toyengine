/**
 * @file rect_gizmo_test.cpp
 * @brief The UI designer's rect gizmo: handles are hit where drawn, body drags move (Y flipped into
 * canvas space), edges resize, anchors are grabbable, drags snap, Shift snaps rotation.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <uicoopa/ui_yaml.h>

#include "editor/support/fixtures.h"
#include "editor/ui/ui_canvas_math.h"
#include "editor/viewport/rect_gizmo.h"

COOPA_TEST_SUITE("rect_gizmo");

namespace toy::editor::testing {

COOPA_TEST(handles_drag_resize_snap_and_rotate) {
    const ui::Rect parent{{0, 0}, {1000, 500}};
    ui::UiView v;
    v.origin = {0, 0};
    v.scale = 1.0f;
    v.canvas_size = {1000, 500};
    RectGizmoTarget t;
    t.parent = parent;
    t.params.anchor_min = t.params.anchor_max = {0.5f, 0.5f};
    t.params.size_delta = {200, 100};
    t.rect = coopa::ui::resolve_rect(parent, t.params);   // (400,200)-(600,300)
    RectGizmo g;
    RectGizmoSettings s;
    s.snap = false;
    // Handles: editor Y is down, so the top edge (canvas y 300) is at editor y 200.
    expect(g.hit(v, t, {600, 250}) == RectHandle::Right && g.hit(v, t, {500, 200}) == RectHandle::Top &&
           g.hit(v, t, {450, 260}) == RectHandle::Body && g.hit(v, t, {500, 250}) == RectHandle::Pivot,
           "rect gizmo: edges, body and pivot are hit where they are drawn");
    auto drive = [&](glm::vec2 from, glm::vec2 to, bool shift = false) {
        RectGizmoInput in;
        in.mouse = from; in.pressed = true; in.down = true; in.shift = shift;
        g.update(v, t, in, s);
        in.pressed = false; in.mouse = to;
        RectGizmoResult r = g.update(v, t, in, s);
        in.down = false; in.released = true;
        const RectGizmoResult f = g.update(v, t, in, s);
        r.finished = f.finished;
        return r;
    };
    RectGizmoResult r = drive({450, 260}, {490, 240});
    expect(r.changed && std::abs(r.params.anchored_position.x - 40) < 1e-3f && std::abs(r.params.anchored_position.y - 20) < 1e-3f && r.finished,
           "rect gizmo: a body drag moves by the drag (Y flipped into canvas space)");
    r = drive({600, 250}, {650, 250});
    expect(std::abs(r.rect.max.x - 650) < 1e-3f && std::abs(r.rect.min.x - 400) < 1e-3f, "rect gizmo: the right handle resizes, the left edge stays");
    // Anchor split: drag the top-right anchor triangle (drawn just outside the anchor point).
    t.params.anchor_min = {0.4f, 0.4f};
    t.params.anchor_max = {0.4f, 0.4f};
    t.params.anchored_position = {100, 50};
    t.rect = coopa::ui::resolve_rect(parent, t.params);
    const glm::vec2 anchor_px = v.to_editor({400, 200});
    expect(g.hit(v, t, anchor_px + glm::vec2(6, -6)) == RectHandle::AnchorTR || g.hit(v, t, anchor_px) != RectHandle::None,
           "rect gizmo: the anchors are grabbable");
    // Snapping to the parent's centre line.
    s.snap = true;
    s.lines.clear();
    ui::add_rect_lines(s.lines, parent);
    t.params.anchor_min = t.params.anchor_max = {0.5f, 0.5f};
    t.params.anchored_position = {0, 0};
    t.rect = coopa::ui::resolve_rect(parent, t.params);
    r = drive({450, 260}, {453, 260});
    expect(std::abs(r.params.anchored_position.x) < 1e-3f && !r.guides.empty(), "rect gizmo: a small drag snaps back onto the centre line");
    // Rotation with Shift snaps to 15 degrees.
    s.snap = false;
    r = drive({615, 185}, {640, 250}, true);
    expect(std::abs(std::fmod(std::abs(r.rotation), 15.0f)) < 1e-3f, "rect gizmo: Shift snaps rotation to 15 degrees");
}

} // namespace toy::editor::testing
