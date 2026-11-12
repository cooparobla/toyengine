/**
 * @file modal_transform_test.cpp
 * @brief Blender-style G / R / S: typed per-axis values, plane locks, local axes, MMB auto axis, edge
 * slide, and Ctrl increment snap.
 */

#include <coopa/testing/test.h>

#include "editor/support/view_helpers.h"
#include "editor/viewport/modal_transform.h"

COOPA_TEST_SUITE("modal_transform");

namespace toy::editor::testing {

/** @brief Axis locking: per-component typed input, plane typing, local scale, MMB auto axis, edge slide. */
COOPA_TEST(axis_locks_typed_values_and_mmb_auto_axis) {
    using coopa::input::Key;
    using coopa::input::Mods;
    const ViewProj vp = test_view_proj();
    const glm::vec2 c = *vp.project(glm::vec3(0));
    ModalTransform mt;
    auto run = [&](std::vector<coopa::input::KeyEvent> keys) {
        mt.update(vp, c, keys, false, false, false, false);
        return mt.update(vp, c, {key_press(Key::Enter)}, false, false, false, false);
    };
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    expect(run({key_press(Key::Z, Mods::Shift), key_press(Key::Num1), key_press(Key::Tab), key_press(Key::Num2)}) ==
               ModalTransform::Outcome::Confirmed &&
               glm::distance(mt.result().translate, glm::vec3(1, 2, 0)) < 1e-5f,
           "G Shift+Z 1 Tab 2 moves (1, 2, 0) in the XY plane");
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    run({key_press(Key::Num1), key_press(Key::Tab), key_press(Key::Num2), key_press(Key::Tab), key_press(Key::Num3)});
    expect(glm::distance(mt.result().translate, glm::vec3(1, 2, 3)) < 1e-5f, "free G 1 Tab 2 Tab 3 moves (1, 2, 3)");

    // A basis rotated 90 degrees about Z: local X is world Y.
    const glm::mat3 rot(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1));
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), rot, c, std::nullopt, "local");
    run({key_press(Key::X), key_press(Key::X), key_press(Key::Num2)});
    expect(glm::distance(mt.result().translate, glm::vec3(0, 2, 0)) < 1e-5f, "G X X 2 moves along the local X axis");
    mt.begin(ModalKind::Scale, vp, glm::vec3(0), rot, c, std::nullopt, "local");
    run({key_press(Key::X), key_press(Key::X), key_press(Key::Num2)});
    const glm::mat4 sm = delta_matrix(glm::vec3(0), glm::vec3(0, 0, 1), 0.0f, mt.result().scale, glm::vec3(0), mt.result().scale_basis);
    expect(glm::distance(glm::vec3(sm * glm::vec4(0, 1, 0, 1)), glm::vec3(0, 2, 0)) < 1e-5f &&
               glm::distance(glm::vec3(sm * glm::vec4(1, 0, 0, 1)), glm::vec3(1, 0, 0)) < 1e-5f,
           "S X X 2 scales along the local axis only");

    // MMB auto constraint: a horizontal drag picks X (screen right is +X from this view), a vertical one Z.
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    mt.update(vp, c, {}, false, false, false, false, true, true);
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, true, false);
    expect(mt.axis() == 0, "MMB drag sideways locks X (got " + std::to_string(mt.axis()) + ")");
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, false, false);
    mt.update(vp, c + glm::vec2(60, 2), {}, false, false, false, false, true, true);
    mt.update(vp, c + glm::vec2(62, -60), {}, false, false, false, false, true, false);
    expect(mt.axis() == 2, "MMB drag up locks Z");
    expect(mt.guide_axes().size() == 1, "an axis constraint draws one guide line");
    mt.update(vp, c, {key_press(Key::Y, Mods::Shift)}, false, false, false, false);
    expect(mt.guide_axes().size() == 2, "a plane constraint draws its two axes");
    mt.cancel();

    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c, std::nullopt, "local", true);
    expect(mt.update(vp, c, {key_press(Key::G)}, false, false, false, false) == ModalTransform::Outcome::SwitchToSlide,
           "G during an edit-mode Grab asks for Edge Slide");
    mt.begin(ModalKind::EdgeSlide, vp, glm::vec3(0), glm::mat3(1.0f), c, glm::vec3(1, 0, 0));
    run({key_press(Key::Period), key_press(Key::Num5)});
    expect(std::abs(mt.result().amount - 0.5f) < 1e-5f, "Edge Slide takes a typed factor");
}

/** @brief Ctrl during a free Grab snaps the move to the viewport grid's increment (snap_step). */
COOPA_TEST(ctrl_snaps_a_free_move_to_the_increment) {
    const ViewProj vp = test_view_proj();
    const glm::vec2 c = *vp.project(glm::vec3(0));
    ModalTransform mt;
    mt.snap_step = 0.05f;
    mt.begin(ModalKind::Grab, vp, glm::vec3(0), glm::mat3(1.0f), c);
    mt.update(vp, c + glm::vec2(37.0f, -11.0f), {}, false, false, /*ctrl=*/true, false);
    const glm::vec3 t = mt.result().translate;
    bool on_grid = glm::length(t) > 0.0f;
    for (int i = 0; i < 3; ++i) on_grid &= std::abs(t[i] / 0.05f - std::round(t[i] / 0.05f)) < 1e-3f;
    expect(on_grid, "Ctrl snaps a free move to the 0.05 increment");
    mt.cancel();
}

} // namespace toy::editor::testing
