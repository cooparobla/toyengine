/**
 * @file outliner_test.cpp
 * @brief The Hierarchy's row toggles, driven as Blender's are: a click acts on one object, a
 * Shift+click on it and every descendant, and holding the button while the pointer crosses other
 * rows copies that state onto every toggle in the same column. The eye is editor-only (the
 * document keeps its `active` flag); the monitor writes the flag, a whole subtree as one
 * undoable step.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("outliner");

namespace toy::editor::testing {

namespace {

glm::vec2 center_of(const imm::Box& b) { return {b.x + b.w * 0.5f, b.y + b.h * 0.5f}; }

}  // namespace

/**
 * @brief The eye column: one row per click, the row's subtree with Shift, and every row the
 *        pointer is dragged across.
 */
COOPA_TEST(the_eye_hides_one_row_a_subtree_with_shift_and_every_row_a_drag_crosses) {
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const ObjectId cube = session.named("cube");
    const ObjectId ground = session.named("ground");
    // A child under the cube, so Shift has something to reach. Its own row stays collapsed:
    // the gesture is on the parent's toggle.
    const ObjectId child = app.document().add_object(app.document().make_object("hat"), cube);
    app.sync().rebuild(engine, app.document());
    tick(engine, 2);
    coopa::test::require(cube && ground && child, "the default scene's cube and ground, plus a child");

    auto eye_of = [&](ObjectId id) {
        auto b = app.outliner_eye_rect(id);
        coopa::test::require(b.has_value(), "the row has an eye toggle");
        return center_of(*b);
    };
    auto shown = [&](ObjectId id) {
        auto* live = app.sync().live(id);
        return live && live->active();
    };

    // One click, one object: the cube's child is not touched.
    in.click(eye_of(cube));
    tick(engine, 2);
    expect(!shown(cube) && shown(child), "a click on the eye hides that object alone");
    in.click(eye_of(cube));
    tick(engine, 2);
    expect(shown(cube) && shown(child), "clicking it again reveals it");

    // Shift: the row and its whole subtree.
    in.click(eye_of(cube), MouseButton::Left, Mods::Shift);
    tick(engine, 2);
    expect(!shown(cube) && !shown(child), "Shift+click hides the object and its children");
    in.click(eye_of(cube), MouseButton::Left, Mods::Shift);
    tick(engine, 2);
    expect(shown(cube) && shown(child), "and Shift+click reveals them again");
    expect(get_bool(*app.document().find(cube), "active", false), "the eye never writes the saved `active` flag");

    // Drag down the column: every eye the pointer crosses takes the state the press set.
    in.move(eye_of(ground));
    in.drag(eye_of(cube), MouseButton::Left);
    tick(engine, 2);
    expect(!shown(ground) && !shown(cube), "a drag across the eye column copies the state onto every row it hits");
    expect(shown(child), "without Shift the drag reaches the rows themselves, not their children");
}

/**
 * @brief The monitor column writes the document's `active` flag -- with the same gestures, and a
 *        subtree (or a drag) as ONE undo step.
 */
COOPA_TEST(the_monitor_writes_active_on_a_subtree_as_one_undo_step) {
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const ObjectId cube = session.named("cube");
    const ObjectId child = app.document().add_object(app.document().make_object("hat"), cube);
    app.sync().rebuild(engine, app.document());
    tick(engine, 2);
    coopa::test::require(cube && child, "the default scene's cube, plus a child");

    // The monitor sits one square right of the eye (draw_outliner_row_ places the pair).
    auto monitor_of = [&](ObjectId id) {
        auto b = app.outliner_eye_rect(id);
        coopa::test::require(b.has_value(), "the row has its toggles");
        return center_of(*b) + glm::vec2(b->w + 2.0f, 0.0f);
    };
    auto active = [&](ObjectId id) { return get_bool(*app.document().find(id), "active", true); };

    in.click(monitor_of(cube));
    tick(engine, 2);
    expect(!active(cube) && active(child), "a click on the monitor disables that object alone");
    in.click(monitor_of(cube));
    tick(engine, 2);
    expect(active(cube) && active(child), "clicking it again enables it");

    in.click(monitor_of(cube), MouseButton::Left, Mods::Shift);
    tick(engine, 2);
    expect(!active(cube) && !active(child), "Shift+click disables the object and its children");
    app.document().undo();
    expect(active(cube) && active(child), "and the subtree is one undo step");
}

}  // namespace toy::editor::testing
