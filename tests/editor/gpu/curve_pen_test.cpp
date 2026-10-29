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

}  // namespace toy::editor::testing
