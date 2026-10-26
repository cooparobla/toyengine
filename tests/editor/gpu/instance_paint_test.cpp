/**
 * @file instance_paint_test.cpp
 * @brief Instance Paint Mode through real input: a painter is added, the mode entered, a stroke
 *        dragged over a terrain places instances as ONE undo step in the scene's timeline, undo
 *        restores the exact records, Erase removes them, Snap lands them on a grid, Place drops
 *        exactly one, and saving the scene writes the painter's `.instances` file.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <set>

#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_painter_system.h>
#include <toyengine/instance/instance_set.h>
#include <toyengine/world/heightmap_store.h>
#include <toyengine/world/heightmap_terrain.h>

#include "editor/instance/instance_select.h"

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("instance_paint");

namespace toy::editor::testing {

using coopa::test::expect_near;
using coopa::test::require;
using toy::instance::InstancePainter;
using toy::instance::InstanceRecord;
using toy::instance::InstanceSetData;
using toy::instance::InstanceSetStore;

namespace {

/** @brief Adds a terrain to paint on and a painter holding one cube prototype. */
struct Bench {
    ObjectId terrain = 0;
    ObjectId painter = 0;
    std::shared_ptr<InstanceSetData> data;
    glm::vec2 size{0.0f};
    float height = 0.0f;
};

Bench make_bench(EditorSession& session) {
    Bench b;
    auto& engine = session.engine;
    auto& app = session.app;

    b.terrain = app.create_with_component("HeightmapTerrain", "Ground");
    tick(engine, 6);
    auto* t = engine.scene().find_first_component<toy::world::HeightmapTerrain>();
    if (t == nullptr || t->data == nullptr) return b;
    b.size = t->data->geometry().size;
    b.height = t->data->sample_height(b.size * 0.5f);

    b.painter = app.create_with_component("InstancePainter", "Scatter");
    tick(engine, 6);
    // One prototype: the cube primitive, which every project has. A bare `mesh:` prototype is
    // enough here -- resolving an object asset is the engine suite's business.
    const int index = app.document().find_component(b.painter, "InstancePainter");
    const Node* obj = app.document().find(b.painter);
    if (index < 0 || obj == nullptr) return b;
    Node comp = obj->at("components").as_seq()[static_cast<std::size_t>(index)];
    Node protos = Node::sequence();
    Node proto = Node::mapping();
    proto["mesh"] = std::string("primitives/cube");
    proto["name"] = std::string("block");
    proto["collision"] = std::string("none");
    protos.as_seq().push_back(std::move(proto));
    comp["prototypes"] = std::move(protos);
    app.document().set_component(b.painter, index, comp, "Set Prototypes");
    // The document is the source of truth; the live scene is derived from it, and a component
    // edit has to be applied for the running painter to see its new palette.
    app.sync().apply(engine, app.document(), {ChangeScope::Object, b.painter});
    tick(engine, 10);

    auto* p = engine.scene().find_first_component<InstancePainter>();
    if (p != nullptr) b.data = p->data;
    return b;
}

std::size_t total(const std::shared_ptr<InstanceSetData>& d) { return d ? d->size() : 0; }

std::vector<InstanceRecord> snapshot(const std::shared_ptr<InstanceSetData>& d) {
    std::vector<InstanceRecord> all;
    if (!d) return all;
    for (auto c : d->chunks()) {
        const auto* recs = d->records(c);
        if (recs != nullptr) all.insert(all.end(), recs->begin(), recs->end());
    }
    return all;
}

} // namespace

COOPA_TEST(instance_paint_mode_paints_erases_undoes_and_saves) {
    using coopa::input::MouseButton;
    toy::world::HeightmapStore::instance().clear();
    InstanceSetStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const Bench bench = make_bench(session);
    require(bench.terrain != 0 && bench.painter != 0, "Add > Instance Painter creates a painter object");
    require(bench.data != nullptr, "the painter's records are live through the store");
    auto* p = engine.scene().find_first_component<InstancePainter>();
    require(p != nullptr && p->prototypes.size() == 1, "its palette has the one prototype");

    app.document().select(bench.terrain);
    app.frame_selected();
    tick(engine, 30);

    app.set_instance_paint_mode(false);   // nothing selected yet: must refuse, not crash
    app.document().select(bench.painter);
    tick(engine, 2);
    app.set_instance_paint_mode(true);
    tick(engine, 2);
    expect(app.instance_paint_mode(), "Instance Paint Mode entered");

    auto& brush = app.instance_brush();
    brush.tool = InstanceTool::Paint;
    brush.radius = 12.0f;
    brush.density = 0.05f;
    brush.min_spacing = 2.0f;
    brush.prototype = 0;

    // A stroke across the middle of the terrain (terrain-local metres -> window pixels).
    const auto a = screen_of(engine, app, bench.terrain,
                             glm::vec3(bench.size.x * 0.4f, bench.size.y * 0.5f, bench.height), in.scale);
    const auto b = screen_of(engine, app, bench.terrain,
                             glm::vec3(bench.size.x * 0.6f, bench.size.y * 0.5f, bench.height), in.scale);
    require(a.has_value() && b.has_value(), "the stroke lies on screen");

    const std::size_t undo0 = app.instance_history().undo_count();
    in.move(*a);
    in.drag(*b, MouseButton::Left, 12);
    tick(engine, 3);

    const std::size_t painted = total(bench.data);
    expect(painted > 0, "the stroke placed instances (" + std::to_string(painted) + ")");
    expect(app.instance_history().undo_count() == undo0 + 1,
           "a whole stroke is ONE undo step, however many dabs it was made of");

    // Minimum spacing is part of the density model: without it a drag piles instances up.
    const auto records = snapshot(bench.data);
    bool spaced = true;
    for (std::size_t i = 0; i < records.size() && spaced; ++i) {
        for (std::size_t j = i + 1; j < records.size() && spaced; ++j) {
            spaced = glm::length(glm::vec2(records[i].position) - glm::vec2(records[j].position)) >
                     brush.min_spacing - 0.01f;
        }
    }
    expect(spaced, "no two painted instances are closer than Min Spacing");

    // Every instance landed ON the ground, not at the brush centre's height.
    auto* t = engine.scene().find_first_component<toy::world::HeightmapTerrain>();
    require(t != nullptr, "the terrain is still there");
    t->sync_field();
    bool on_ground = true;
    for (const InstanceRecord& r : records) {
        const float ground = t->field_view().height(r.position);
        on_ground = on_ground && std::abs(r.position.z - ground) < 0.3f;
    }
    expect(on_ground,
           "each instance sits on the surface under IT, not at the height of the brush's centre "
           "-- every candidate is raycast separately");
    dump(engine, "instance_paint_stroke");

    app.undo();
    tick(engine, 3);
    expect(total(bench.data) == 0, "undo removes exactly the stroke");
    app.redo();
    tick(engine, 3);
    expect(total(bench.data) == painted, "redo puts all of them back");

    // Erase.
    brush.tool = InstanceTool::Erase;
    in.move(*a);
    in.drag(*b, MouseButton::Left, 12);
    tick(engine, 3);
    expect(total(bench.data) < painted,
           "an Erase stroke removes instances (" + std::to_string(total(bench.data)) + " left)");
    expect(app.instance_history().undo_count() == undo0 + 2, "...as one more undo step");

    // Saving the scene writes the records to their own file, as the terrain's PNGs are written.
    expect(bench.data->unsaved(), "the painted records are unsaved");
    const auto scene_path = session.project.assets() / "scenes" / "paint_test" / "scene.yaml";
    std::filesystem::create_directories(scene_path.parent_path());
    expect(app.save_scene_as(scene_path), "the scene saves");
    const std::string file = InstanceSetStore::instance().path_of(*bench.data);
    expect(!file.empty() && std::filesystem::exists(file),
           "saving the scene writes the painter's .instances file beside it (" + file + ")");
    expect(!bench.data->unsaved(), "...and the records are clean afterwards");

    app.set_instance_paint_mode(false);
    tick(engine, 2);
    expect(!app.instance_paint_mode(), "Instance Paint Mode exited");
    InstanceSetStore::instance().clear();
    toy::world::HeightmapStore::instance().clear();
}

COOPA_TEST(place_drops_one_and_snap_puts_it_on_the_grid) {
    using coopa::input::MouseButton;
    toy::world::HeightmapStore::instance().clear();
    InstanceSetStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const Bench bench = make_bench(session);
    require(bench.data != nullptr, "the bench is set up");
    app.document().select(bench.terrain);
    app.frame_selected();
    tick(engine, 30);
    app.document().select(bench.painter);
    app.set_instance_paint_mode(true);
    tick(engine, 2);
    require(app.instance_paint_mode(), "Instance Paint Mode entered");

    auto& brush = app.instance_brush();
    brush.tool = InstanceTool::Place;
    brush.min_spacing = 0.0f;
    brush.snap = true;
    brush.grid_cell = 4.0f;
    brush.grid_origin = glm::vec2(0.0f);
    brush.yaw_step = 90.0f;

    // Three clicks at points a metre or two apart, which snapping should fold onto cell centres.
    int clicks = 0;
    for (const glm::vec2 offset : {glm::vec2(0.0f), glm::vec2(1.5f, 0.0f), glm::vec2(9.0f, 3.0f)}) {
        const auto at = screen_of(engine, app, bench.terrain,
                                  glm::vec3(bench.size.x * 0.5f + offset.x, bench.size.y * 0.5f + offset.y,
                                            bench.height),
                                  in.scale);
        if (!at) continue;
        in.click(*at, MouseButton::Left);
        tick(engine, 3);
        ++clicks;
    }
    require(clicks == 3, "all three clicks landed on screen");

    const auto records = snapshot(bench.data);
    expect(!records.empty(), "clicking with Place drops instances");
    expect(records.size() <= 3,
           "Place drops at most one per click -- never a brushful (" + std::to_string(records.size()) + ")");

    // One Per Cell: the first two clicks are 1.5 m apart inside one 4 m cell, so the second is
    // refused; the third is in a different cell and lands.
    std::set<std::pair<int, int>> cells;
    bool on_grid = true;
    for (const InstanceRecord& r : records) {
        const float cx = r.position.x / brush.grid_cell, cy = r.position.y / brush.grid_cell;
        // A snapped position sits at a cell CENTRE: its coordinate over the cell size is n + 0.5.
        on_grid = on_grid && std::abs(cx - std::floor(cx) - 0.5f) < 0.02f &&
                  std::abs(cy - std::floor(cy) - 0.5f) < 0.02f;
        cells.insert({static_cast<int>(std::floor(cx)), static_cast<int>(std::floor(cy))});
    }
    expect(on_grid,
           "with Snap on, every instance is at the centre of a grid cell -- which is what makes a "
           "fence or a wall line up");
    expect(cells.size() == records.size(),
           "One Per Cell means no cell holds two, so the two clicks inside one cell produced one "
           "instance (" + std::to_string(records.size()) + " in " + std::to_string(cells.size()) + " cells)");

    app.set_instance_paint_mode(false);
    InstanceSetStore::instance().clear();
    toy::world::HeightmapStore::instance().clear();
}

/**
 * @brief Editing a prototype must not crash the renderer.
 *
 * The editor applies a component edit in its `post_late_update` hook, which runs BETWEEN the
 * scene update (where the painter system builds its chunks) and the render hand-off (where it
 * gives the renderer their batches). That rebuild destroys the `InstancePainter`, so anything
 * the system remembered about it across that gap is freed memory by the time the renderer
 * asks -- which segfaulted on the first frame of dragging any prototype field.
 */
COOPA_TEST(editing_a_prototype_while_the_scene_runs_does_not_crash) {
    toy::world::HeightmapStore::instance().clear();
    InstanceSetStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;

    const Bench bench = make_bench(session);
    require(bench.data != nullptr, "the bench is set up");
    app.document().select(bench.terrain);
    app.frame_selected();
    tick(engine, 30);
    app.document().select(bench.painter);
    app.set_instance_paint_mode(true);
    tick(engine, 2);
    require(app.instance_paint_mode(), "Instance Paint Mode entered");

    // Something painted, so the system has chunks and batches to hand over.
    auto& brush = app.instance_brush();
    brush.tool = InstanceTool::Paint;
    brush.radius = 10.0f;
    brush.density = 0.08f;
    brush.min_spacing = 1.0f;
    const auto at = screen_of(engine, app, bench.terrain,
                              glm::vec3(bench.size.x * 0.5f, bench.size.y * 0.5f, bench.height), session.in.scale);
    require(at.has_value(), "the brush point is on screen");
    session.in.move(*at);
    session.in.drag(*at + glm::vec2(40.0f, 0.0f), coopa::input::MouseButton::Left, 8);
    tick(engine, 4);
    const std::size_t painted = total(bench.data);
    expect(painted > 0, "something was painted (" + std::to_string(painted) + ")");

    // Now drag the Scale value the way the inspector does: a component edit every frame.
    const int index = app.document().find_component(bench.painter, "InstancePainter");
    require(index >= 0, "the painter component is in the document");
    for (int frame = 0; frame < 12; ++frame) {
        const Node* obj = app.document().find(bench.painter);
        require(obj != nullptr, "the painter object survives each edit");
        Node comp = obj->at("components").as_seq()[static_cast<std::size_t>(index)];
        (void)comp;
        // Exactly what dragging the palette's Scale widget does -- and crucially through the
        // editor's DEFERRED apply, so the object is rebuilt inside the frame, after the painter
        // system ran and before the renderer asks it for batches.
        const float lo = 1.0f + 0.05f * static_cast<float>(frame);
        app.edit_instance_component("Instance Scale Range", [&](Node& c) {
            Node r = Node::sequence();
            r.as_seq().push_back(Node(lo));
            r.as_seq().push_back(Node(lo + 0.5f));
            c["prototypes"].as_seq()[0]["scale_range"] = std::move(r);
        }, "ip_scale");
        tick(engine, 1);   // ...and a full frame, so the rebuild and the render hand-off both run
    }

    expect(total(bench.data) == painted,
           "editing a prototype changes how FUTURE instances are placed, never the records "
           "already painted");
    auto* p = engine.scene().find_first_component<InstancePainter>();
    require(p != nullptr, "the painter is still live after a dozen rebuilds");

    // The decisive check: every renderer proxy handed over must belong to the component that is
    // alive RIGHT NOW. A proxy from the pre-edit component is freed memory the renderer would
    // then dereference -- the actual segfault, which cannot be caught by waiting for a fault
    // (a use-after-free reads plausible, still-mapped bytes more often than it traps).
    // The last edit's rebuild landed in post_late_update, AFTER the painter system ran, so its
    // prototypes are not resolved yet. A couple of frames let the drag settle the way releasing
    // the mouse does -- and are themselves the proof that the rebuild/hand-off gap is survivable.
    tick(engine, 6);
    auto* sys = toy::instance::find_instance_painter_system(engine.scene());
    require(sys != nullptr, "the painter system is installed");
    std::set<const void*> live_proxies;
    for (const auto& proto : p->prototypes) {
        for (const auto& part : proto.parts) live_proxies.insert(part.proxy);
    }
    toy::render::ParticleFrameState state;
    sys->collect_render(engine.scene(), glm::vec3(0.0f, 0.0f, 20.0f), {}, state);
    expect(!state.meshes.empty(),
           "once the drag settles it is drawing again -- and the chunks were never thrown away "
           "to get there, which is what would blink a painted world out on every frame of it");
    bool proxies_live = true;
    for (const auto& b : state.meshes) {
        proxies_live = proxies_live && live_proxies.count(b.proxy) == 1;
    }
    expect(proxies_live,
           "every batch's proxy belongs to the CURRENT component -- nothing cached across the "
           "editor's rebuild, which happens between the scene update and the render hand-off");
    expect_near(p->prototypes.at(0).scale_range.x, 1.55f, 1e-3f,
                "the last edit reached the live component");
    expect(app.instance_paint_mode(), "and the mode survived them all");

    app.set_instance_paint_mode(false);
    InstanceSetStore::instance().clear();
    toy::world::HeightmapStore::instance().clear();
}

/**
 * @brief The Select tool through real input: click one, box-drag several, turn them, delete.
 */
COOPA_TEST(select_picks_instances_by_click_and_by_box_then_edits_them) {
    using coopa::input::MouseButton;
    toy::world::HeightmapStore::instance().clear();
    InstanceSetStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const Bench bench = make_bench(session);
    require(bench.data != nullptr, "the bench is set up");
    app.document().select(bench.terrain);
    app.frame_selected();
    tick(engine, 30);
    app.document().select(bench.painter);
    app.set_instance_paint_mode(true);
    tick(engine, 2);
    require(app.instance_paint_mode(), "Instance Paint Mode entered");

    // Place a known row of instances with the Place tool, so their positions are exact.
    auto& brush = app.instance_brush();
    brush.tool = InstanceTool::Place;
    brush.snap = false;
    brush.min_spacing = 0.0f;
    std::vector<glm::vec2> screen_of_each;
    for (int i = 0; i < 5; ++i) {
        const auto at = screen_of(engine, app, bench.terrain,
                                  glm::vec3(bench.size.x * 0.5f + 3.0f * static_cast<float>(i),
                                            bench.size.y * 0.5f, bench.height),
                                  in.scale);
        require(at.has_value(), "each placement point is on screen");
        in.click(*at, MouseButton::Left);
        tick(engine, 3);
        screen_of_each.push_back(*at);
    }
    require(total(bench.data) == 5, "five instances were placed (" + std::to_string(total(bench.data)) + ")");

    // --- click to select one ---
    brush.tool = InstanceTool::Select;
    tick(engine, 2);
    in.click(screen_of_each[2], MouseButton::Left);
    tick(engine, 3);
    expect(app.instance_selection_size() == 1,
           "clicking an instance selects exactly it (" + std::to_string(app.instance_selection_size()) + ")");

    // Clicking well away from anything, with no modifier, clears the selection.
    in.click(screen_of_each[0] + glm::vec2(0.0f, -220.0f), MouseButton::Left);
    tick(engine, 3);
    expect(app.instance_selection_size() == 0, "clicking empty space clears the selection");

    // --- drag a box over several ---
    const glm::vec2 lo = screen_of_each.front() - glm::vec2(40.0f, 60.0f);
    const glm::vec2 hi = screen_of_each.back() + glm::vec2(40.0f, 60.0f);
    in.move(lo);
    in.drag(hi, MouseButton::Left, 8);
    tick(engine, 3);
    const std::size_t boxed = app.instance_selection_size();
    expect(boxed == 5,
           "a box drag selects everything whose screen position falls inside it (" +
               std::to_string(boxed) + " of 5)");

    // --- turn the group, as the Selection panel does ---
    std::vector<float> yaw_before;
    for (auto c : bench.data->chunks()) {
        if (const auto* recs = bench.data->records(c)) {
            for (const auto& r : *recs) yaw_before.push_back(instance_yaw(r));
        }
    }
    const std::size_t undo_before = app.instance_history().undo_count();
    app.turn_instance_selection(glm::radians(90.0f));
    tick(engine, 3);
    expect(app.instance_history().undo_count() == undo_before + 1,
           "turning the selection is ONE undo step, however many instances it held");

    std::vector<float> yaw_after;
    for (auto c : bench.data->chunks()) {
        if (const auto* recs = bench.data->records(c)) {
            for (const auto& r : *recs) yaw_after.push_back(instance_yaw(r));
        }
    }
    require(yaw_before.size() == yaw_after.size(), "the same instances are still there");
    int turned = 0;
    for (std::size_t i = 0; i < yaw_after.size(); ++i) {
        if (std::abs(yaw_after[i] - yaw_before[i]) > 0.01f) ++turned;
    }
    expect(turned == 5, "every selected instance turned (" + std::to_string(turned) + " of 5)");

    app.undo();
    tick(engine, 3);
    expect(app.instance_selection_size() == 0,
           "an undo changes the records under the selection, so the selection is dropped rather "
           "than left addressing whatever moved into those indices");
    expect(total(bench.data) == 5, "...and the instances themselves are all still there");

    // --- select again and delete ---
    in.move(lo);
    in.drag(hi, MouseButton::Left, 8);
    tick(engine, 3);
    expect(app.instance_selection_size() == 5, "the box selects them again");
    app.delete_instance_selection();
    tick(engine, 3);
    expect(total(bench.data) == 0, "deleting the selection removes every one of them");
    expect(app.instance_selection_size() == 0, "...and leaves nothing selected");

    app.undo();
    tick(engine, 3);
    expect(total(bench.data) == 5, "undo brings them back");

    app.set_instance_paint_mode(false);
    InstanceSetStore::instance().clear();
    toy::world::HeightmapStore::instance().clear();
}

} // namespace toy::editor::testing
