/**
 * @file ui_canvas_math_test.cpp
 * @brief The UI designer's rect math (ui_canvas_math.h): resolve, editor <-> canvas mapping, resize
 * with Alt / Shift, anchors / pivot / presets that keep the rect, snapping, and RectTransform
 * blocks read exactly as the engine reads them.
 */

#include <coopa/testing/test.h>

#include <uicoopa/ui_yaml.h>

#include "editor/support/fixtures.h"
#include "editor/ui/ui_canvas_math.h"

COOPA_TEST_SUITE("ui_canvas_math");

namespace toy::editor::testing {

namespace {

bool near_rect(const ui::Rect& a, const ui::Rect& b, float eps = 1e-3f) {
    return glm::all(glm::lessThan(glm::abs(a.min - b.min), glm::vec2(eps))) && glm::all(glm::lessThan(glm::abs(a.max - b.max), glm::vec2(eps)));
}

} // namespace

COOPA_TEST(resize_anchors_pivot_presets_and_snap) {
    const ui::Rect parent{{0, 0}, {1000, 500}};
    ui::RectParams p;
    p.anchor_min = p.anchor_max = {0.5f, 0.5f};
    p.pivot = {0.5f, 0.5f};
    p.size_delta = {200, 100};
    p.anchored_position = {50, 20};
    const ui::Rect r0 = coopa::ui::resolve_rect(parent, p);
    expect(near_rect(r0, {{450, 220}, {650, 320}}), "ui math: a centred rect resolves where expected");

    // View mapping is an exact round trip, with Y flipped (canvas up, editor down).
    ui::UiView v;
    v.origin = {100, 50};
    v.scale = 0.5f;
    v.canvas_size = {1000, 500};
    const glm::vec2 e = v.to_editor({250, 100});
    expect(glm::distance(e, glm::vec2(225, 250)) < 1e-4f && glm::distance(v.to_canvas(e), glm::vec2(250, 100)) < 1e-4f,
           "ui math: editor <-> canvas mapping round-trips with Y flipped");

    // Resize from the right edge keeps the left edge; from a corner with Alt keeps the centre.
    const ui::Rect rr = ui::resize_rect(r0, 1, 0, {30, 0}, false, false);
    expect(std::abs(rr.min.x - r0.min.x) < 1e-4f && std::abs(rr.max.x - (r0.max.x + 30)) < 1e-4f && rr.min.y == r0.min.y,
           "ui math: resizing the right edge keeps the left edge");
    const ui::Rect rs = ui::resize_rect(r0, 1, 1, {20, 10}, true, false);
    expect(glm::distance(rs.center(), r0.center()) < 1e-3f && std::abs(rs.size().x - (r0.size().x + 40)) < 1e-3f,
           "ui math: Alt resizes symmetrically about the centre");
    const ui::Rect ra = ui::resize_rect(r0, 1, 1, {100, 0}, false, true);
    expect(std::abs(ra.size().x / ra.size().y - r0.size().x / r0.size().y) < 1e-3f, "ui math: Shift keeps the aspect ratio");
    const ui::Rect inv = ui::resize_rect(r0, -1, 0, {500, 0}, false, false);
    expect(inv.size().x >= 1.0f && std::abs(inv.max.x - r0.max.x) < 1e-4f, "ui math: a resize never inverts the rect");

    // set_rect / anchors / pivot all keep the rect where it is.
    ui::RectParams q = p;
    ui::set_rect(parent, q, rr);
    expect(near_rect(coopa::ui::resolve_rect(parent, q), rr), "ui math: set_rect() re-expresses a rect exactly");
    q = p;
    ui::set_anchors_keep_rect(parent, q, {0, 0}, {1, 1});
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0), "ui math: changing anchors keeps the rect on screen");
    ui::set_pivot_keep_rect(parent, q, {0, 1});
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0) && q.pivot == glm::vec2(0, 1), "ui math: moving the pivot keeps the rect");
    // ...and the stretched rect follows its parent when the parent grows.
    const ui::Rect big{{0, 0}, {2000, 1000}};
    const ui::Rect stretched = coopa::ui::resolve_rect(big, q);
    expect(std::abs(stretched.size().x - (r0.size().x + 1000)) < 1e-3f, "ui math: stretch anchors grow with the parent");

    // Presets: Unity's picker (rect stays; Shift pivot; Alt snaps).
    const auto& presets = ui::anchor_presets();
    q = p;
    const ui::AnchorPresetInfo* tl = nullptr;
    for (const auto& pr : presets) if (std::string(pr.name) == "TopLeft") tl = &pr;
    ASSERT_TRUE(tl != nullptr);
    ui::apply_anchor_preset(parent, q, *tl, false, false);
    expect(near_rect(coopa::ui::resolve_rect(parent, q), r0) && q.anchor_min == glm::vec2(0, 1), "ui math: a preset alone keeps the rect");
    ui::apply_anchor_preset(parent, q, *tl, true, true);
    expect(q.pivot == glm::vec2(0, 1) && q.anchored_position == glm::vec2(0) &&
           near_rect(coopa::ui::resolve_rect(parent, q), {{0, 400}, {200, 500}}), "ui math: Shift+Alt preset snaps into the top-left corner");
    expect(ui::matching_preset(q) && std::string(ui::matching_preset(q)->name) == "TopLeft", "ui math: the current preset is recognised");

    // Snapping: an edge within the threshold moves onto the target and reports a guide.
    std::vector<ui::SnapLine> lines;
    ui::add_rect_lines(lines, parent);
    const bool all[2][3] = {{true, true, true}, {true, true, true}};
    std::vector<ui::SnapLine> hit;
    const glm::vec2 d = ui::snap_rect({{3, 100}, {103, 150}}, lines, 5.0f, all, &hit);
    expect(std::abs(d.x + 3.0f) < 1e-4f && !hit.empty(), "ui math: an edge 3 px from the parent's snaps to it");
}

COOPA_TEST(rect_blocks_read_like_the_engine_and_normalise) {
    Node blk = fkyaml::node::deserialize(std::string(
        "{type: RectTransform, anchor_preset: StretchAll, offset_min: {x: 10, y: 20}, offset_max: {x: -30, y: -40}, rotation: 15}"));
    const coopa::ui::RectTransform rt = ui::parse_rect_block(blk);
    coopa::ui::RectTransform engine_rt;
    coopa::ui::detail::parse_rect_transform(blk, engine_rt);
    const ui::Rect parent{{0, 0}, {800, 600}};
    expect(near_rect(coopa::ui::resolve_rect(parent, rt.params()), coopa::ui::resolve_rect(parent, engine_rt.params())),
           "ui block: the editor reads a RectTransform exactly as the engine does (preset + offsets)");
    Node out = blk;
    ui::write_rect_params(out, rt.params());
    expect(!out.contains("anchor_preset") && !out.contains("offset_min") && out.contains("anchor_min") && out.contains("size_delta") &&
           out.contains("rotation"), "ui block: writing normalises to anchors/pivot/position/size (rotation kept)");
    coopa::ui::RectTransform again;
    coopa::ui::detail::parse_rect_transform(out, again);
    expect(near_rect(coopa::ui::resolve_rect(parent, again.params()), coopa::ui::resolve_rect(parent, engine_rt.params())),
           "ui block: ...and resolves to the same rect");
}

} // namespace toy::editor::testing
