/**
 * @file gizmo_test.cpp
 * @brief The translate gizmo: hover makes an axis hot, a drag moves along that axis only by the
 * cursor's world distance, Ctrl snaps, release ends the drag.
 */

#include <coopa/testing/test.h>

#include "editor/support/view_helpers.h"

COOPA_TEST_SUITE("gizmo");

namespace toy::editor::testing {

COOPA_TEST(translate_drag_moves_along_the_axis_and_snaps) {
    const ViewProj vp = test_view_proj();
    Gizmo g;
    g.mode = GizmoMode::Translate;
    const glm::vec3 pivot(0.0f);
    const auto c = *vp.project(pivot);
    const auto tip = *vp.project(pivot + glm::vec3(1, 0, 0) * g.size_px * vp.world_per_pixel(pivot));
    const glm::vec2 grab = c + (tip - c) * 0.6f;
    GizmoDelta r = g.update(vp, pivot, glm::mat3(1.0f), grab, false, false, false, false, true);
    expect(g.hot_axis() == 0, "hovering the X handle makes it hot");
    r = g.update(vp, pivot, glm::mat3(1.0f), grab, true, true, false, false, true);
    expect(r.started && r.active, "pressing starts a drag");
    // Move the mouse to where world x = +2 projects.
    const auto target = *vp.project(glm::vec3(2, 0, 0)) + (grab - c);
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, true, false, false, true);
    expect(std::abs(r.translate.x - 2.0f) < 0.05f && std::abs(r.translate.y) < 1e-4f && std::abs(r.translate.z) < 1e-4f,
           "dragging along X moves only X, by the cursor's world distance (" + std::to_string(r.translate.x) + ")");
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, true, false, true, true);
    expect(std::abs(r.translate.x - 2.0f) < 1e-4f, "Ctrl snaps to 0.25 units");
    r = g.update(vp, pivot, glm::mat3(1.0f), target, false, false, true, false, true);
    expect(r.finished && !g.dragging(), "release ends the drag");
}

} // namespace toy::editor::testing
