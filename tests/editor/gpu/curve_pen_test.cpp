/**
 * @file curve_pen_test.cpp
 * @brief The Pen tool end to end, and a curve driving a procedural graph.
 *
 * The geometry is unit-tested without a device (`curve_data`, `curve_edit`). What needs a real
 * EditorApp is the wiring: that entering the mode adds a Curve, that clicks in the viewport
 * reach the document, and that a `{curve: ...}` binding makes a bake follow the drawn path.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <functional>
#include <algorithm>
#include <string>

#include <toyengine/curve/curve.h>
#include <toyengine/instance/instance_painter.h>

#include "editor/app/ui/curve_pen.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("curve_pen");

namespace toy::editor::testing {
namespace {

namespace fs = std::filesystem;
namespace cv = toy::curve;

/// @brief A scene with one empty object to draw on.
const char* k_scene =
    "scene:\n"
    "  scene_name: pen\n"
    "  root_objects:\n"
    "    - name: Path\n"
    "      active: true\n"
    "      components:\n"
    "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n";

} // namespace

/** @brief Entering Pen Mode on an object with no path gives it one. */
COOPA_TEST(entering_pen_mode_adds_a_curve_when_the_object_has_none) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    expect(path != 0, "curve_pen: the object is there");

    expect(s.app.curve_pen().begin_pen_(path), "curve_pen: the mode opened");
    tick(s.engine, 4);

    expect(s.app.curve_pen().in_pen_mode_(), "curve_pen: and it is active");
    auto* live = s.app.sync().live(path);
    expect(live && live->get_component<cv::Curve>() != nullptr,
           "curve_pen: a Curve component was added -- entering the tool on a bare object means "
           "'draw a path here', not 'refuse'");

    s.app.curve_pen().end_pen_();
    expect(!s.app.curve_pen().in_pen_mode_(), "curve_pen: and leaving it closes the mode");
}

/** @brief Clicks in the viewport add points, and they reach the document. */
COOPA_TEST(clicking_in_the_viewport_draws_the_curve) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    s.app.document().select(path);
    expect(s.app.curve_pen().begin_pen_(path), "curve_pen: the mode opened");
    tick(s.engine, 6);

    auto point_count = [&]() -> std::size_t {
        const cv::CurveData* c = s.app.curve_pen().curve();
        return c ? c->points.size() : 0;
    };
    expect(point_count() == 0, "curve_pen: it starts empty");

    // Three clicks across the middle of the viewport, well clear of the panels.
    for (glm::vec2 at : {glm::vec2(480.0f, 260.0f), glm::vec2(600.0f, 300.0f),
                         glm::vec2(700.0f, 240.0f)}) {
        s.in.click(at);
        tick(s.engine, 3);
    }

    expect(point_count() == 3,
           "curve_pen: three clicks made three control points (" +
               std::to_string(point_count()) + ")");

    // And it is in the DOCUMENT, not only on the live object -- otherwise a save loses the path.
    const Node* obj = s.app.document().find(path);
    const Node* comp = nullptr;
    for (const auto& c : obj->at("components").as_seq()) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == "Curve") comp = &c;
    }
    expect(comp != nullptr, "curve_pen: the document has the Curve component");
    expect(comp->contains("points") && comp->at("points").size() == 3,
           "curve_pen: with the three points, so saving the scene keeps them");

    dump(s.engine, "curve_pen_drawn");
    s.app.curve_pen().end_pen_();
}

/** @brief A graph bound to a curve instances along the drawn path. */
COOPA_TEST(a_graph_bound_to_a_curve_follows_the_drawn_path) {
    EditorSession s({.prepare = [](Project& project) {
        const fs::path engine = fs::path(ROOT_DIR) / "assets";
        fs::create_directories(project.assets() / "graphs");
        fs::create_directories(project.assets() / "meshes" / "props");
        fs::copy_file(engine / "graphs" / "curve_railing.yaml",
                      project.assets() / "graphs" / "curve_railing.yaml",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                      project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                      fs::copy_options::overwrite_existing);
        write_text(project.assets() / "scenes" / "curved" / "scene.yaml",
            "scene:\n"
            "  scene_name: curved\n"
            "  root_objects:\n"
            "    - name: Path\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: Curve\n"
            "          closed: false\n"
            "          resolution: 12\n"
            "          points:\n"
            "            - {p: [-4, 0, 0], mode: auto}\n"
            "            - {p: [0, 3, 0], mode: auto}\n"
            "            - {p: [4, 0, 0], mode: auto}\n"
            "    - name: Railing\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/curve_railing\n"
            "          bake: on_load\n"
            "          inputs:\n"
            "            path: {curve: Path}\n"
            "            posts: 10\n"
            "            height: 1.2\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "curved" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId railing = s.named("Railing");
    expect(railing != 0, "curve_pen: the railing is there");
    tick(s.engine, 24);   // the bake is a job; its publish lands on a later frame

    // Every published instance, so their spread can be measured against the curve's.
    std::vector<glm::vec3> placed;
    std::function<void(const coopa::scene::SceneObject&)> walk =
        [&](const coopa::scene::SceneObject& o) {
            if (const auto* p = o.get_component<toy::instance::InstancePainter>(); p && p->data) {
                for (const auto& c : p->data->chunks()) {
                    if (const auto* recs = p->data->records(c)) {
                        for (const auto& r : *recs) placed.push_back(r.position);
                    }
                }
            }
            for (const auto& ch : o.children()) walk(*ch);
        };
    if (auto* live = s.app.sync().live(railing)) walk(*live);

    expect(placed.size() == 10,
           "curve_pen: ten posts were placed along the curve (" +
               std::to_string(placed.size()) + ")");

    // The curve arches to y=3 at its middle. Instancing along a STRAIGHT line between the ends
    // would leave every post at y=0, so a raised one proves the path was followed.
    float highest = 0.0f;
    for (const glm::vec3& p : placed) highest = std::max(highest, p.y);
    expect(highest > 1.0f,
           "curve_pen: a post sits well off the straight line between the ends (y=" +
               std::to_string(highest) + "), so the bake followed the CURVE rather than its chord");

    dump(s.engine, "curve_railing_baked");
}


/**
 * @brief The Pen is reachable from the viewport TOOLBAR, by clicking it.
 *
 * It was only in the mode dropdown at first, which is not where a tool is looked for -- and
 * `interaction_mode()` masked `mode_` unless a MESH was being edited, so the menu's tick never
 * lit either. Both are invisible failures: the feature exists, builds, and cannot be found.
 * Driving a real click at the button's place on screen is the only thing that proves otherwise.
 */
COOPA_TEST(the_pen_tool_is_on_the_viewport_toolbar_and_a_click_enters_it) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    s.app.document().select(path);
    tick(s.engine, 6);

    expect(!s.app.curve_pen().in_pen_mode_(), "curve_pen: it does not start in the mode");

    // The toolbar is a column down the left of the viewport. Sweep its buttons rather than
    // hard-coding one y: the row order is layout, not behaviour, and a test that pins it would
    // break every time a tool is inserted above.
    bool entered = false;
    for (float y = 60.0f; y < 300.0f && !entered; y += 6.0f) {
        // Re-select every probe: a click that misses the toolbar lands in the viewport on empty
        // space and DESELECTS, and the Pen correctly does nothing with no object to draw on --
        // so one stray click would make every later probe a no-op and the sweep useless.
        s.app.document().select(path);
        s.in.click({286.0f, y});
        tick(s.engine, 2);
        entered = s.app.curve_pen().in_pen_mode_();
    }
    expect(entered,
           "curve_pen: a click on the viewport toolbar entered Pen Mode -- if this fails the "
           "button is not drawn, not hit-testable, or not wired");

    expect(s.app.interaction_mode() == InteractionMode::Pen,
           "curve_pen: and the editor REPORTS Pen Mode, so the header and the mode menu agree "
           "with what the tool is actually doing");

    dump(s.engine, "curve_pen_toolbar");
}


/**
 * @brief Switching back to Object Mode LEAVES the Pen, and clicks stop drawing.
 *
 * The Pen takes viewport clicks before picking does, so a mode switch that left it running kept
 * drawing points with nothing on screen explaining why. Asserting the flag is not enough -- the
 * symptom was clicks, so the test clicks.
 */
COOPA_TEST(leaving_pen_mode_stops_it_drawing) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    s.app.document().select(path);
    expect(s.app.curve_pen().begin_pen_(path), "curve_pen: the mode opened");
    tick(s.engine, 4);

    s.in.click({520.0f, 280.0f});
    tick(s.engine, 3);
    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 1, "curve_pen: a click while in the mode drew a point");

    s.app.set_interaction_mode(InteractionMode::Object);
    tick(s.engine, 4);
    expect(!s.app.curve_pen().in_pen_mode_(),
           "curve_pen: Object Mode left the Pen");
    expect(s.app.interaction_mode() == InteractionMode::Object,
           "curve_pen: and the editor agrees it is in Object Mode");

    // The real symptom: clicks must stop reaching the pen.
    const std::size_t before = s.app.curve_pen().curve() ? s.app.curve_pen().curve()->points.size()
                                                         : [&] {
        const Node* obj = s.app.document().find(path);
        for (const auto& comp : obj->at("components").as_seq()) {
            if (comp.contains("type") && comp.at("type").get_value<std::string>() == "Curve") {
                return comp.contains("points") ? comp.at("points").size() : std::size_t(0);
            }
        }
        return std::size_t(0);
    }();
    s.in.click({620.0f, 320.0f});
    s.in.click({660.0f, 300.0f});
    tick(s.engine, 4);

    std::size_t after = 0;
    const Node* obj = s.app.document().find(path);
    for (const auto& comp : obj->at("components").as_seq()) {
        if (comp.contains("type") && comp.at("type").get_value<std::string>() == "Curve") {
            after = comp.contains("points") ? comp.at("points").size() : 0;
        }
    }
    expect(after == before,
           "curve_pen: clicks after leaving add NO points (" + std::to_string(before) + " -> " +
               std::to_string(after) + ") -- the bug was the pen still drawing in Object Mode");
}

/** @brief G moves the selected anchor, with axis locking, and Escape puts it back. */
COOPA_TEST(g_grabs_a_curve_anchor_with_axis_locking) {
    using coopa::input::Key;
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    s.app.document().select(path);
    expect(s.app.curve_pen().begin_pen_(path), "curve_pen: the mode opened");
    tick(s.engine, 4);

    for (glm::vec2 at : {glm::vec2(500.0f, 300.0f), glm::vec2(620.0f, 280.0f)}) {
        s.in.click(at);
        tick(s.engine, 3);
    }
    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 2, "curve_pen: two points drawn");
    expect(s.app.curve_pen().selected_point() == 1, "curve_pen: the last one is selected");
    const glm::vec3 before = c->points[1].position;

    // G, then X to lock to the world X axis, then move and confirm.
    s.in.key(Key::G);
    tick(s.engine, 2);
    expect(s.app.curve_pen().transforming(), "curve_pen: G started a grab");
    s.in.key(Key::X);
    tick(s.engine, 2);
    s.in.move({760.0f, 280.0f}, 2);
    s.in.click({760.0f, 280.0f});
    tick(s.engine, 3);

    expect(!s.app.curve_pen().transforming(), "curve_pen: the click confirmed it");
    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(after && after->points.size() == 2, "curve_pen: still two points");
    const glm::vec3 moved = after->points[1].position;
    expect(glm::distance(moved, before) > 0.05f,
           "curve_pen: the anchor moved (" + std::to_string(glm::distance(moved, before)) + ")");
    expect(std::abs(moved.y - before.y) < 1e-3f && std::abs(moved.z - before.z) < 1e-3f,
           "curve_pen: along X ONLY -- the axis lock held, which is the whole point of reusing "
           "the editor's own modal rather than writing a second one");
}

/** @brief R swings an anchor's handles about it without moving the anchor. */
COOPA_TEST(r_rotates_an_anchors_handles_about_the_anchor) {
    using coopa::input::Key;
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    s.app.document().select(path);
    expect(s.app.curve_pen().begin_pen_(path), "curve_pen: the mode opened");
    tick(s.engine, 4);

    for (glm::vec2 at : {glm::vec2(480.0f, 300.0f), glm::vec2(580.0f, 280.0f),
                         glm::vec2(680.0f, 310.0f)}) {
        s.in.click(at);
        tick(s.engine, 3);
    }
    s.app.curve_pen().select(1);
    tick(s.engine, 2);

    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 3, "curve_pen: three points drawn");
    const glm::vec3 anchor_before = c->points[1].position;
    const glm::vec3 out_before = c->points[1].out_handle;

    s.in.key(Key::R);
    tick(s.engine, 2);
    expect(s.app.curve_pen().transforming(), "curve_pen: R started a rotate");
    s.in.move({580.0f, 200.0f}, 3);
    s.in.click({580.0f, 200.0f});
    tick(s.engine, 3);

    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(after != nullptr, "curve_pen: the curve survived");
    expect(glm::distance(after->points[1].position, anchor_before) < 1e-4f,
           "curve_pen: the ANCHOR did not move -- a rotate pivots about it");
    expect(glm::distance(after->points[1].out_handle, out_before) > 1e-3f,
           "curve_pen: but its handle swung, which is what aims the tangent");
}


namespace {

/// @brief Enters Pen Mode on `path` and draws `n` anchors across the viewport.
void draw_anchors(EditorSession& s, ObjectId path, int n) {
    s.app.document().select(path);
    s.app.curve_pen().begin_pen_(path);
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Draw);
    tick(s.engine, 4);
    for (int i = 0; i < n; ++i) {
        s.in.click({480.0f + 70.0f * static_cast<float>(i), 290.0f});
        tick(s.engine, 3);
    }
}

} // namespace

/**
 * @brief The Select tool picks anchors without drawing; the Draw tool draws.
 *
 * The mode/tool split: Pen MODE says you are editing anchors, the TOOL says what a click does.
 * Before this, every click drew -- so there was no way to pick an anchor without adding one.
 */
COOPA_TEST(the_select_tool_picks_anchors_instead_of_drawing_them) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 3);

    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 3, "curve_pen: the Draw tool drew three anchors");

    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);
    tick(s.engine, 2);

    // A click on empty space under Select must NOT add a point.
    s.in.click({700.0f, 480.0f});
    tick(s.engine, 3);
    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(after && after->points.size() == 3,
           "curve_pen: the Select tool does not draw (" +
               std::to_string(after ? after->points.size() : 0) + " anchors)");
    expect(s.app.curve_pen().selection().empty(),
           "curve_pen: and a click on nothing clears the selection");
}

/** @brief Shift+click builds a multi-anchor selection, and G moves all of it. */
COOPA_TEST(several_anchors_can_be_selected_and_moved_together) {
    using coopa::input::Key;
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 3);
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);

    s.app.curve_pen().select(0);
    s.app.curve_pen().select_add(2);
    tick(s.engine, 2);
    expect(s.app.curve_pen().selection().size() == 2, "curve_pen: two anchors selected");

    const cv::CurveData before = *s.app.curve_pen().curve();

    s.in.key(Key::G);
    tick(s.engine, 2);
    expect(s.app.curve_pen().transforming(), "curve_pen: G started a grab");
    s.in.key(Key::X);
    tick(s.engine, 2);
    s.in.move({820.0f, 290.0f}, 2);
    s.in.click({820.0f, 290.0f});
    tick(s.engine, 3);

    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(after && after->points.size() == 3, "curve_pen: still three anchors");
    const float moved0 = after->points[0].position.x - before.points[0].position.x;
    const float moved2 = after->points[2].position.x - before.points[2].position.x;
    const float moved1 = after->points[1].position.x - before.points[1].position.x;
    expect(std::abs(moved0) > 0.05f && std::abs(moved2) > 0.05f,
           "curve_pen: BOTH selected anchors moved (" + std::to_string(moved0) + ", " +
               std::to_string(moved2) + ")");
    expect(std::abs(moved0 - moved2) < 1e-3f,
           "curve_pen: by the same amount -- one grab, not two");
    expect(std::abs(moved1) < 1e-3f,
           "curve_pen: and the unselected one between them stayed put");
}

/**
 * @brief E makes a new anchor from the selected one and hands it straight to the grab.
 *
 * Mesh extrude's shape, and it works under EITHER tool: the mode is what decides you are
 * editing anchors.
 */
COOPA_TEST(e_extrudes_a_new_anchor_and_starts_moving_it) {
    using coopa::input::Key;
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 2);
    // Deliberately under the SELECT tool, to show E is a mode operator and not a pen one.
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);
    s.app.curve_pen().select(1);
    tick(s.engine, 2);

    s.in.key(Key::E);
    tick(s.engine, 2);

    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 3,
           "curve_pen: E added an anchor (" + std::to_string(c ? c->points.size() : 0) + ")");
    expect(s.app.curve_pen().transforming(),
           "curve_pen: and handed it straight to a grab, so it can be placed without starting "
           "one by hand");
    expect(s.app.curve_pen().selection().size() == 1 &&
               s.app.curve_pen().selected_point() == 2,
           "curve_pen: the NEW anchor is what is selected and being moved");

    s.in.move({760.0f, 200.0f}, 2);
    s.in.click({760.0f, 200.0f});
    tick(s.engine, 3);
    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(!s.app.curve_pen().transforming(), "curve_pen: the click confirmed the move");
    expect(glm::distance(after->points[2].position, after->points[1].position) > 0.05f,
           "curve_pen: and the new anchor ended up away from its parent rather than on top of it");

    dump(s.engine, "curve_pen_extruded");
}

/** @brief E needs exactly one anchor: from two there is no "which way does it continue". */
COOPA_TEST(e_refuses_unless_exactly_one_anchor_is_selected) {
    using coopa::input::Key;
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 3);
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);

    s.app.curve_pen().select(0);
    s.app.curve_pen().select_add(1);
    tick(s.engine, 2);
    s.in.key(Key::E);
    tick(s.engine, 3);

    const cv::CurveData* c = s.app.curve_pen().curve();
    expect(c && c->points.size() == 3, "curve_pen: E with two selected added nothing");
    expect(!s.app.curve_pen().transforming(), "curve_pen: and started no grab");
}


/**
 * @brief Clicking an anchor does not TELEPORT it to the cursor.
 *
 * A drag moves what it grabbed by how far the mouse travels. Computing `mouse - anchor` on the
 * first frame instead snaps the anchor under the cursor the instant it is touched -- which looks
 * like the select tool flinging whatever you click at the mouse.
 */
COOPA_TEST(clicking_an_anchor_does_not_move_it) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 3);
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);
    tick(s.engine, 3);

    const cv::CurveData before = *s.app.curve_pen().curve();

    // Click the FIRST anchor from a cursor position well away from it: a missing grab offset
    // would drag it across to wherever the click landed.
    const auto px = screen_of(s.engine, s.app, path, before.points[0].position,
                              std::max(1.0f, s.engine.display_scale()));
    expect(px.has_value(), "curve_pen: the anchor projects on screen");
    s.in.click(*px);
    tick(s.engine, 3);

    const cv::CurveData* after = s.app.curve_pen().curve();
    expect(after && after->points.size() == 3, "curve_pen: no anchor was added or lost");
    expect(s.app.curve_pen().selected_point() == 0, "curve_pen: the clicked anchor is selected");
    for (std::size_t i = 0; i < 3; ++i) {
        expect(glm::distance(after->points[i].position, before.points[i].position) < 1e-3f,
               "curve_pen: anchor " + std::to_string(i) + " did not move -- a click selects, it "
               "does not drag");
    }
}

/** @brief Dragging a box over anchors selects them, as Box Select does on objects. */
COOPA_TEST(the_select_tool_box_selects_anchors) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "pen" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "pen" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId path = s.named("Path");
    draw_anchors(s, path, 3);   // drawn left to right across the viewport
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);
    s.app.curve_pen().select(-1);
    tick(s.engine, 3);

    const cv::CurveData before = *s.app.curve_pen().curve();
    const float scale = std::max(1.0f, s.engine.display_scale());
    const auto a0 = screen_of(s.engine, s.app, path, before.points[0].position, scale);
    const auto a1 = screen_of(s.engine, s.app, path, before.points[1].position, scale);
    const auto a2 = screen_of(s.engine, s.app, path, before.points[2].position, scale);
    expect(a0 && a1 && a2, "curve_pen: all three anchors project");

    // A band around the first two, stopping short of the third.
    const float cut = (a1->x + a2->x) * 0.5f;
    s.in.move({a0->x - 30.0f, a0->y - 60.0f}, 1);
    s.in.drag({cut, a0->y + 60.0f}, coopa::input::MouseButton::Left, 6);
    tick(s.engine, 3);

    const auto& sel = s.app.curve_pen().selection();
    expect(sel.size() == 2,
           "curve_pen: the band selected two anchors (" + std::to_string(sel.size()) + ")");
    expect(std::find(sel.begin(), sel.end(), std::size_t(2)) == sel.end(),
           "curve_pen: and not the one outside it");

    const cv::CurveData* after = s.app.curve_pen().curve();
    for (std::size_t i = 0; i < 3; ++i) {
        expect(glm::distance(after->points[i].position, before.points[i].position) < 1e-3f,
               "curve_pen: box-selecting moved nothing");
    }
}


/**
 * @brief Editing a curve RE-BAKES the graph that follows it.
 *
 * The whole point of a `{curve: ...}` binding. `bake: on_change` compares an input hash each
 * frame, and `Value::hash_into()` folds a CONSTANT for any type with no registered content hash
 * -- so `Curve` reported "unchanged" forever and dragging an anchor updated nothing, with a
 * perfectly correct first bake to hide it.
 */
COOPA_TEST(editing_the_curve_rebakes_the_graph_that_follows_it) {
    EditorSession s({.prepare = [](Project& project) {
        const fs::path engine = fs::path(ROOT_DIR) / "assets";
        fs::create_directories(project.assets() / "graphs");
        fs::create_directories(project.assets() / "meshes" / "props");
        fs::copy_file(engine / "graphs" / "curve_railing.yaml",
                      project.assets() / "graphs" / "curve_railing.yaml",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                      project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                      fs::copy_options::overwrite_existing);
        write_text(project.assets() / "scenes" / "follow" / "scene.yaml",
            "scene:\n"
            "  scene_name: follow\n"
            "  root_objects:\n"
            "    - name: Path\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: Curve\n"
            "          resolution: 10\n"
            "          points:\n"
            "            - {p: [-4, 0, 0], mode: auto}\n"
            "            - {p: [0, 0, 0], mode: auto}\n"
            "            - {p: [4, 0, 0], mode: auto}\n"
            "    - name: Railing\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/curve_railing\n"
            "          bake: on_change\n"
            "          inputs:\n"
            "            path: {curve: Path}\n"
            "            posts: 6\n"
            "            height: 1.0\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "follow" / "scene.yaml"),
           "curve_pen: the scene opens");
    const ObjectId railing = s.named("Railing");
    const ObjectId path = s.named("Path");
    expect(railing != 0 && path != 0, "curve_pen: both objects are there");
    tick(s.engine, 24);

    // Where the posts stand, so a re-bake can be seen in the GEOMETRY rather than in a counter.
    auto post_spread = [&]() -> float {
        coopa::scene::SceneObject* live = s.app.sync().live(railing);
        if (live == nullptr) return -1.0f;
        float lo = 1e30f, hi = -1e30f;
        std::function<void(const coopa::scene::SceneObject&)> walk =
            [&](const coopa::scene::SceneObject& o) {
                if (const auto* p = o.get_component<toy::instance::InstancePainter>(); p && p->data) {
                    for (const auto& c : p->data->chunks()) {
                        if (const auto* recs = p->data->records(c)) {
                            for (const auto& r : *recs) {
                                lo = std::min(lo, r.position.y);
                                hi = std::max(hi, r.position.y);
                            }
                        }
                    }
                }
                for (const auto& ch : o.children()) walk(*ch);
            };
        walk(*live);
        return hi > lo ? hi - lo : 0.0f;
    };

    const float flat = post_spread();
    expect(flat >= 0.0f, "curve_pen: the first bake published posts");
    expect(flat < 0.5f, "curve_pen: along a straight path they sit in a line (spread " +
                            std::to_string(flat) + ")");

    // Drag the middle anchor well off the line, the way the Pen tool does.
    s.app.curve_pen().begin_pen_(path);
    s.app.curve_pen().select(1);
    tick(s.engine, 3);
    Node* obj = s.app.document().find(path);
    int index = 0;
    for (const auto& comp : obj->at("components").as_seq()) {
        if (comp.contains("type") && comp.at("type").get_value<std::string>() == "Curve") break;
        ++index;
    }
    Node edited = obj->at("components").as_seq()[static_cast<std::size_t>(index)];
    Node moved = Node::sequence();
    moved.as_seq().push_back(Node(0.0));
    moved.as_seq().push_back(Node(4.0));   // y: straight off the old line
    moved.as_seq().push_back(Node(0.0));
    edited["points"].as_seq()[1]["p"] = std::move(moved);
    s.app.apply_scene_change_for_test(path, index, std::move(edited));
    tick(s.engine, 30);

    const float bent = post_spread();
    expect(bent > flat + 0.5f,
           "curve_pen: the posts followed the moved anchor (spread " + std::to_string(flat) +
               " -> " + std::to_string(bent) + "). Equal spreads mean the graph never noticed "
               "the curve changed");
}

}  // namespace toy::editor::testing
