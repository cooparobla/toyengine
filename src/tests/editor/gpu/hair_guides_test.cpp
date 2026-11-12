/**
 * @file hair_guides_test.cpp
 * @brief A groom's guides, edited through Pen Mode: entering, switching between curves, and the
 *        add / duplicate / delete the Hair inspector offers.
 *
 * Guides are ordinary `Curve` objects, so they need no mode of their own -- Pen Mode edits one
 * curve at a time and its Select tool switches to whichever curve you click. What that leaves
 * worth testing is the wiring nobody can see: that entering on a Hair component lands on a guide
 * rather than putting a stray Curve on the scalp, that switching really moves what clicks edit,
 * and that deleting leaves the Pen somewhere valid.
 *
 * Each of those fails silently -- the viewport looks right and you edit the wrong curve, or none.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <toyengine/curve/curve.h>

#include "editor/app/ui/curve_pen.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("hair_guides");

namespace toy::editor::testing {
namespace {

namespace fs = std::filesystem;
namespace cv = toy::curve;

/// A groom with no guides -- the state a Hair component starts in -- plus a plain curve
/// elsewhere in the scene, so "switch to any curve" has something unrelated to switch to.
const char* k_scene =
    "scene:\n"
    "  scene_name: hairguides\n"
    "  root_objects:\n"
    "    - name: Head\n"
    "      active: true\n"
    "      components:\n"
    "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
    "        - {type: MeshRenderer, mesh_path: sphere}\n"
    "        - {type: Hair, density: 40.0, length: 0.3, segments: 4}\n";

/// The scalp lifted clear of the world ground plane -- Z is up in this engine
/// (editor/viewport/curve_edit.cpp's point_on_ground tests the Z plane, and hair_demo's own
/// scalps sit at z ~ 1.2, not y) -- so a guide point that lands on the mesh and one that lands
/// on the ground behind it cannot be confused for each other.
const char* k_scene_elevated =
    "scene:\n"
    "  scene_name: hairguides\n"
    "  root_objects:\n"
    "    - name: Head\n"
    "      active: true\n"
    "      components:\n"
    "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 5.0}}\n"
    "        - {type: MeshRenderer, mesh_path: sphere}\n"
    "        - {type: Hair, density: 40.0, length: 0.3, segments: 4}\n";

std::size_t points_of(EditorSession& s, ObjectId curve) {
    auto* live = s.app.sync().live(curve);
    if (live == nullptr) return 0;
    const auto* c = live->get_component<cv::Curve>();
    return c ? c->data.points.size() : 0;
}

/// Point `index` of `curve`'s shape, in world space.
std::optional<glm::vec3> world_point_of(EditorSession& s, ObjectId curve, std::size_t index) {
    auto* live = s.app.sync().live(curve);
    if (live == nullptr) return std::nullopt;
    const auto* c = live->get_component<cv::Curve>();
    const auto* tc = live->get_transform();
    if (c == nullptr || tc == nullptr || index >= c->data.points.size()) return std::nullopt;
    const glm::mat4 world = tc->transform().get_world_matrix();
    return glm::vec3(world * glm::vec4(c->data.points[index].position, 1.0f));
}

/// The guide objects under `head`'s guide group, in document order.
std::vector<ObjectId> guides_of(EditorSession& s, ObjectId head) {
    std::vector<ObjectId> out;
    const Node* obj = s.app.document().find(head);
    if (obj == nullptr || !obj->contains("children")) return out;
    for (const Node& group : obj->at("children")) {
        if (!group.contains("children")) continue;
        for (const Node& child : group.at("children")) {
            out.push_back(SceneDocument::id_of(child));
        }
    }
    return out;
}

} // namespace

/** @brief Pen Mode on a groom edits a guide, not the scalp -- creating the first one if needed. */
COOPA_TEST(entering_pen_mode_on_a_groom_lands_on_a_guide) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "hairguides" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "hairguides" / "scene.yaml"),
           "hair_guides: the scene opens");
    const ObjectId head = s.named("Head");
    expect(head != 0, "hair_guides: the groom is there");
    expect(guides_of(s, head).empty(), "hair_guides: and starts with no guides");

    expect(s.app.curve_pen().begin_pen_(head), "hair_guides: Pen Mode opened on it");
    tick(s.engine, 4);
    expect(s.app.curve_pen().in_pen_mode_(), "hair_guides: and is active");

    const std::vector<ObjectId> list = guides_of(s, head);
    expect(list.size() == 1u, "hair_guides: entering made the first guide to draw on");
    expect(s.app.curve_pen().target() == list.front(),
           "hair_guides: and the Pen is on THAT, not on the scalp -- a Hair component holds no "
           "curve, so editing the object you selected would edit nothing");

    // A stray Curve on the scalp would be the failure this redirect exists to prevent.
    auto* live_head = s.app.sync().live(head);
    expect(live_head != nullptr && live_head->get_component<cv::Curve>() == nullptr,
           "hair_guides: and no Curve was added to the scalp itself");

    const Node* obj = s.app.document().find(head);
    const Node* hair = nullptr;
    for (const auto& c : obj->at("components").as_seq()) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == "Hair") hair = &c;
    }
    expect(hair != nullptr && hair->contains("guides"),
           "hair_guides: and the component points at the group -- without that the groom follows "
           "nothing however many curves exist under it");

    s.app.curve_pen().end_pen_();
}

/**
 * @brief A freshly created guide keeps drawing once Pen Mode moves on.
 *
 * `add_hair_guide_()` used to build its Curve from the plain schema default, whose
 * `always_show` is `false` -- fine for a general-purpose path, wrong for a guide, which exists
 * only to be looked at. The guide drew fine while it was the Pen's own target (every curve is
 * visible in Pen Mode so the Select tool has something to click), then vanished the moment
 * editing moved elsewhere, because it was neither the target nor the document selection. Every
 * hand-authored guide in the demo scenes sets `always_show: true` for exactly this reason.
 */
COOPA_TEST(a_new_guide_keeps_drawing_once_pen_mode_moves_on) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "hairguides" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "hairguides" / "scene.yaml"),
           "hair_guides: the scene opens");
    const ObjectId head = s.named("Head");
    expect(s.app.curve_pen().begin_pen_(head), "hair_guides: Pen Mode opened, making the first guide");
    tick(s.engine, 4);
    const ObjectId guide = s.app.curve_pen().target();
    expect(guide != 0, "hair_guides: the guide exists");

    const Node* obj = s.app.document().find(guide);
    const Node* curve = nullptr;
    for (const auto& c : obj->at("components").as_seq()) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == "Curve") curve = &c;
    }
    expect(curve != nullptr && curve->contains("always_show") &&
               curve->at("always_show").get_value<bool>(),
           "hair_guides: its Curve was saved with always_show: true");

    // Leave the mode and deselect everything: the state a guide is left in once you are done
    // editing it, and the one the bug report hit -- not the Pen's target, not selected.
    s.app.curve_pen().end_pen_();
    s.app.document().clear_selection();
    tick(s.engine, 4);
    expect(s.app.scene_curves_drawn() == 1,
           "hair_guides: the guide still draws (drew " +
               std::to_string(s.app.scene_curves_drawn()) + ")");
}

/**
 * @brief A guide point clicked onto the scalp lands ON it, not on the ground plane behind it.
 *
 * place_at_() used to answer every click with a ray/ground-plane intersection at a fixed world
 * height, regardless of what was actually under the cursor -- so a scalp that stands clear of
 * the ground (lifted on a stand, mounted on a wall) was see-through: a click square on its
 * silhouette placed the point on the floor, metres behind the mesh it looked like you were
 * drawing on.
 */
COOPA_TEST(a_guide_point_lands_on_the_scalp_mesh_not_the_ground_behind_it) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "hairguides" / "scene.yaml", k_scene_elevated);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "hairguides" / "scene.yaml"),
           "hair_guides: the scene opens");
    const ObjectId head = s.named("Head");
    expect(head != 0, "hair_guides: the elevated groom is there");
    // open_scene() frames the whole scene, so the scalp -- the only thing in it -- sits centred
    // in the viewport; tick a few frames to let that settle before reading screen coordinates.
    tick(s.engine, 6);

    expect(s.app.curve_pen().begin_pen_(head), "hair_guides: Pen Mode opened on it");
    tick(s.engine, 4);
    const ObjectId guide = s.app.curve_pen().target();
    expect(guide != 0, "hair_guides: landed on a guide to draw");

    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Draw);
    s.in.click(glm::vec2(640.0f, 380.0f));   // dead centre of the 1280 x 760 viewport
    tick(s.engine, 3);
    expect(points_of(s, guide) == 1u, "hair_guides: the click dropped a point");

    const auto p = world_point_of(s, guide, 0);
    expect(p.has_value(), "hair_guides: and it has a position to check");
    if (p) {
        expect(std::abs(p->z - 5.0f) < 1.5f,
               "hair_guides: the point sits near the scalp's own height (z=5), not near the "
               "ground plane at z=0 -- off by " + std::to_string(p->z - 5.0f) + " m");
    }

    s.app.curve_pen().end_pen_();
}

/** @brief The Select tool switches to whichever curve is clicked, guides included. */
COOPA_TEST(clicking_another_curve_in_select_mode_switches_to_it) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "hairguides" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "hairguides" / "scene.yaml"),
           "hair_guides: the scene opens");
    const ObjectId head = s.named("Head");
    s.app.document().select(head);
    expect(s.app.curve_pen().begin_pen_(head), "hair_guides: Pen Mode opened");
    tick(s.engine, 6);

    // Draw the first guide, so it has a line on screen to click later.
    const ObjectId first = s.app.curve_pen().target();
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Draw);
    const glm::vec2 a(470.0f, 250.0f);
    const glm::vec2 b(620.0f, 320.0f);
    for (glm::vec2 at : {a, b}) {
        s.in.click(at);
        tick(s.engine, 3);
    }
    expect(points_of(s, first) == 2u, "hair_guides: the first guide has a line to click on");

    // A second guide, somewhere else entirely.
    const ObjectId second = s.app.curve_pen().add_hair_guide_(head, "Guides");
    tick(s.engine, 4);
    expect(second != 0 && second != first, "hair_guides: a second guide exists");
    expect(s.app.curve_pen().target() == second, "hair_guides: and the Pen moved to it");
    s.in.click(glm::vec2(820.0f, 420.0f));
    tick(s.engine, 3);
    expect(points_of(s, second) == 1u, "hair_guides: drawn on the new one, not the old");

    // Now switch BACK by clicking the first guide's line, under the Select tool.
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Select);
    s.in.click(glm::vec2(0.5f * (a.x + b.x), 0.5f * (a.y + b.y)));
    tick(s.engine, 4);
    expect(s.app.curve_pen().target() == first,
           "hair_guides: clicking a curve's line in Select mode switches the Pen to it -- which "
           "is how you move between a groom's guides without a mode of their own");

    expect(points_of(s, second) == 1u,
           "hair_guides: and the click only switched -- it did not also drop a point");

    s.app.curve_pen().end_pen_();
}

/** @brief Duplicate carries the shape; delete lands the Pen somewhere valid. */
COOPA_TEST(duplicating_copies_the_shape_and_deleting_lands_on_a_neighbour) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "hairguides" / "scene.yaml", k_scene);
    }});
    expect(s.app.open_scene(s.project.assets() / "scenes" / "hairguides" / "scene.yaml"),
           "hair_guides: the scene opens");
    const ObjectId head = s.named("Head");
    s.app.document().select(head);
    s.app.curve_pen().begin_pen_(head);
    tick(s.engine, 4);

    const ObjectId first = s.app.curve_pen().target();
    s.app.curve_pen().set_tool(CurvePenPanel::PenTool::Draw);
    for (glm::vec2 at : {glm::vec2(480.0f, 260.0f), glm::vec2(600.0f, 300.0f)}) {
        s.in.click(at);
        tick(s.engine, 3);
    }
    expect(points_of(s, first) == 2u, "hair_guides: the first guide has a shape to copy");

    const ObjectId copy = s.app.curve_pen().duplicate_hair_guide_(first);
    tick(s.engine, 4);
    expect(copy != 0 && copy != first, "hair_guides: Duplicate made a separate guide");
    expect(points_of(s, copy) == 2u,
           "hair_guides: carrying the original's points -- a copy that came out empty would be "
           "an Add Guide with extra steps");
    expect(s.app.curve_pen().target() == copy, "hair_guides: and the Pen is drawing it");

    expect(guides_of(s, head).size() == 2u, "hair_guides: two guides now");
    s.app.curve_pen().delete_hair_guide_(copy);
    tick(s.engine, 4);
    expect(guides_of(s, head).size() == 1u, "hair_guides: deleting removed it");
    expect(s.app.curve_pen().target() == first,
           "hair_guides: and the Pen landed on the survivor -- a target left on a deleted object "
           "is a dangling edit waiting to happen");

    s.app.curve_pen().delete_hair_guide_(first);
    tick(s.engine, 4);
    expect(guides_of(s, head).empty(), "hair_guides: the last guide can be deleted too");
    expect(s.app.curve_pen().target() == 0,
           "hair_guides: leaving the Pen detached rather than on a destroyed object");

    s.app.curve_pen().end_pen_();
}

} // namespace toy::editor::testing
