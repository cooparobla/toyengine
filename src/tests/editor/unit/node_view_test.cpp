/**
 * @file node_view_test.cpp
 * @brief The node canvas's pan/zoom transform.
 *
 * `imm` has no transform of any kind, so every point on the canvas goes through this by hand.
 * Layout is covered by node_layout_test, picking by node_hit_test, graph edits by node_edit_test.
 */

#include <coopa/testing/test.h>

#include "editor/support/fixtures.h"

#include "editor/node/node_view.h"

COOPA_TEST_SUITE("node_view");

namespace toy::editor::testing {
namespace {

using namespace toy::editor::node;

const imm::Box kViewport{100.0f, 50.0f, 800.0f, 600.0f};   // Deliberately NOT at the origin.

bool near_pt(glm::vec2 a, glm::vec2 b, float eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}

} // namespace

/** @brief The two conversions are inverses, including when the viewport is offset. */
COOPA_TEST(to_screen_and_to_graph_round_trip) {
    NodeView v;
    v.pan  = {120.0f, -40.0f};
    v.zoom = 1.7f;

    for (glm::vec2 p : {glm::vec2{0.0f, 0.0f}, glm::vec2{250.0f, 80.0f}, glm::vec2{-600.0f, 900.0f}}) {
        expect(near_pt(v.to_graph(kViewport, v.to_screen(kViewport, p)), p),
               "node_view: graph -> screen -> graph returns the point it started from");
    }

    expect(near_pt(v.to_screen(kViewport, v.pan), kViewport.center()),
           "node_view: the pan point is the one shown at the viewport's centre -- which is what "
           "lets a zoom about the centre leave pan alone");
}

/** @brief A screen drag converts to graph units by dividing out the zoom, not multiplying. */
COOPA_TEST(a_screen_delta_becomes_a_graph_delta) {
    NodeView v;
    v.zoom = 2.0f;
    expect(near_pt(v.delta_to_graph({20.0f, -10.0f}), {10.0f, -5.0f}),
           "node_view: zoomed 2x, a 20 px drag moves a node 10 graph units -- the node tracks the "
           "pointer rather than running ahead of it");

    v.zoom = 0.5f;
    expect(near_pt(v.delta_to_graph({20.0f, 0.0f}), {40.0f, 0.0f}),
           "node_view: and zoomed out, the same drag covers more graph");
}

/**
 * @brief Zooming keeps the point under the cursor exactly where it was.
 *
 * This is the property that makes a canvas feel right, and the easiest one to get subtly wrong --
 * an off-by-one in the correction drifts the graph out from under the pointer a little per notch,
 * which reads as the canvas "fighting" you.
 */
COOPA_TEST(zooming_keeps_the_point_under_the_cursor_fixed) {
    for (glm::vec2 anchor : {glm::vec2{120.0f, 70.0f}, kViewport.center(),
                             glm::vec2{880.0f, 640.0f}}) {
        NodeView v;
        v.pan  = {33.0f, -17.0f};
        v.zoom = 1.0f;
        const glm::vec2 before = v.to_graph(kViewport, anchor);

        for (int i = 0; i < 6; ++i) v.zoom_at(kViewport, anchor, 1.1f);
        for (int i = 0; i < 3; ++i) v.zoom_at(kViewport, anchor, 1.0f / 1.1f);

        expect(near_pt(v.to_graph(kViewport, anchor), before, 1e-2f),
               "node_view: after nine zoom steps the same graph point is still under the cursor");
    }
}

/** @brief Zoom saturates rather than running away, and the anchor still does not drift. */
COOPA_TEST(zoom_clamps_and_a_saturated_zoom_does_not_drift) {
    NodeView v;
    const glm::vec2 anchor{200.0f, 300.0f};

    for (int i = 0; i < 200; ++i) v.zoom_at(kViewport, anchor, 1.2f);
    expect(v.zoom <= NodeView::k_max_zoom + 1e-4f,
           "node_view: zooming in forever stops at the ceiling");
    const glm::vec2 at_ceiling = v.to_graph(kViewport, anchor);
    v.zoom_at(kViewport, anchor, 1.2f);
    expect(near_pt(v.to_graph(kViewport, anchor), at_ceiling),
           "node_view: a zoom that is entirely clamped away moves nothing. Correcting the pan by "
           "the UNCLAMPED factor would slide the graph sideways every scroll at the limit");

    for (int i = 0; i < 400; ++i) v.zoom_at(kViewport, anchor, 1.0f / 1.2f);
    expect(v.zoom >= NodeView::k_min_zoom - 1e-4f, "node_view: and zooming out stops at the floor");
}

/** @brief Panning moves the content with the pointer. */
COOPA_TEST(panning_moves_the_content_with_the_drag) {
    NodeView v;
    v.zoom = 2.0f;
    const glm::vec2 probe{10.0f, 10.0f};
    const glm::vec2 before = v.to_screen(kViewport, probe);

    v.pan_by({40.0f, 20.0f});
    expect(near_pt(v.to_screen(kViewport, probe), before + glm::vec2(40.0f, 20.0f)),
           "node_view: dragging right by 40 px moves what is on screen right by 40 px, whatever "
           "the zoom");
}

/** @brief Framing fits the content, and the degenerate cases stay usable. */
COOPA_TEST(fitting_centres_the_content_and_survives_a_degenerate_box) {
    const NodeView wide = fit_view(kViewport, {0.0f, 0.0f}, {1600.0f, 1200.0f});
    expect(near_pt(wide.pan, {800.0f, 600.0f}), "node_view: framing centres on the content");
    expect(wide.zoom < 1.0f, "node_view: content larger than the viewport zooms out to fit");
    const glm::vec2 corner = wide.to_screen(kViewport, {0.0f, 0.0f});
    expect(corner.x >= kViewport.x && corner.y >= kViewport.y,
           "node_view: and the far corner lands inside the viewport, margin included");

    // One node: zero span in both axes. A naive viewport/span is a division by zero.
    const NodeView single = fit_view(kViewport, {50.0f, 50.0f}, {50.0f, 50.0f});
    expect(single.zoom > 0.0f && std::isfinite(single.zoom),
           "node_view: framing a single point gives a finite zoom rather than infinity");
    expect(near_pt(single.pan, {50.0f, 50.0f}), "node_view: centred on it");

    // A row of nodes: zero span in Y only, so only X may constrain the zoom.
    const NodeView row = fit_view(kViewport, {0.0f, 100.0f}, {2000.0f, 100.0f});
    expect(row.zoom > 0.0f && std::isfinite(row.zoom) && row.zoom < 1.0f,
           "node_view: a flat row is fitted by its width alone, not by its zero height");

    const NodeView empty = fit_view(kViewport, {0.0f, 0.0f}, {0.0f, 0.0f});
    expect(empty.zoom >= NodeView::k_min_zoom && empty.zoom <= NodeView::k_max_zoom,
           "node_view: frame-all on an empty graph still leaves a usable canvas");
}

}  // namespace toy::editor::testing
