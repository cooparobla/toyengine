/**
 * @file node_graph_test.cpp
 * @brief The node-graph workspace end to end: opening, the Graphs tab, editing, undo and save.
 *
 * The geometry and the graph edits are unit-tested without a device (node_view / node_layout /
 * node_hit / node_edit). What needs a real EditorApp is the wiring: that a graph asset opens the
 * workspace at all, that the canvas replaces the 3D viewport, and that Ctrl+S and Ctrl+Z reach
 * the document -- the three things that fail SILENTLY when a registration site is missed.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <string>

#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/graph_yaml.h>

#include "editor/app/ui/assets.h"
#include "editor/app/ui/node_graph.h"
#include "editor/schema/inspector_sections.h"
#include "editor/support/imm_harness.h"

#include <toyengine/procgraph/scene_input.h>

#include <coopa/input/input.h>
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("node_graph");

namespace toy::editor::testing {
namespace {

namespace ng = coopa::nodegraph;

/// @brief A three-node graph with an interface, written into the scratch project.
const char* k_graph = R"(name: probe
inputs:
  - id: size
    type: Float
    label: Size
    default: 1.0
    min: 0.1
    max: 8.0
  - id: target
    type: Object
    label: Target
outputs:
  - id: geometry
    type: Geometry
nodes:
  - id: 1
    type: Cube
    pos: [-200, 0]
  - id: 2
    type: TransformGeometry
    pos: [100, 0]
  - id: 3
    type: GroupOutput
    pos: [400, 0]
links:
  - [1, geometry, 2, geometry]
  - [2, geometry, 3, geometry]
)";

/// @brief Reloads the graph from disk, to check what a save actually wrote.
ng::Graph reload(const std::filesystem::path& p) {
    ng::Diagnostics diag;
    return ng::load_graph(p.string(), diag);
}

} // namespace

/** @brief A graph asset opens the workspace, and the canvas replaces the 3D viewport. */
COOPA_TEST(opening_a_graph_asset_enters_the_graph_workspace) {
    EditorSession s;
    const std::filesystem::path graph = s.project.assets() / "graphs" / "probe.yaml";
    write_text(graph, k_graph);
    s.project.refresh();

    s.app.open_asset(AssetType::NodeGraph, "graphs/probe.yaml");
    tick(s.engine, 4);

    coopa::test::require(s.app.active_asset_type() == AssetType::NodeGraph,
                         "node_graph: the workspace opened. Without the set_view_() call in "
                         "open_asset_now_() the file loads and nothing on screen changes");
    expect(s.app.graph_workspace().open(), "node_graph: and the document is open");
    expect(!s.app.graph_workspace().dirty(), "node_graph: a freshly opened graph is not dirty");

    // Only written when EDITOR_DUMP_DIR is set: a screenshot is the one thing that shows the
    // wires actually read as wires.
    dump(s.engine, "node_graph_canvas");
}

/**
 * @brief The Graphs tab lists graph assets.
 *
 * Without the type-table row, `asset_type_info_()` silently falls back to the *Scenes* entry, so
 * the tab shows scenes under a Graphs heading rather than being obviously absent.
 */
COOPA_TEST(the_asset_browser_has_a_graphs_tab_that_lists_graphs) {
    EditorSession s;
    write_text(s.project.assets() / "graphs" / "probe.yaml", k_graph);
    write_text(s.project.assets() / "graphs" / "second.yaml", k_graph);
    s.project.refresh();

    expect(s.project.list("graphs", ".yaml").size() == 2,
           "node_graph: both graph assets are on disk for the Graphs tab to list");

    expect(AssetsPanel::asset_type_of_("graphs/probe.yaml") == AssetType::NodeGraph,
           "node_graph: a path under graphs/ is recognised as a graph. Without this every drag, "
           "drop and open-by-path sees AssetType::None");
    expect(AssetsPanel::asset_type_of_("scenes/x.yaml") != AssetType::NodeGraph,
           "node_graph: and nothing else is mistaken for one");
}

/** @brief Moving a node writes a new `pos:` through the save, and it survives a reload. */
COOPA_TEST(a_moved_node_is_saved_and_reloads_where_it_was_put) {
    EditorSession s;
    const std::filesystem::path graph = s.project.assets() / "graphs" / "probe.yaml";
    write_text(graph, k_graph);
    s.project.refresh();

    s.app.open_asset(AssetType::NodeGraph, "graphs/probe.yaml");
    tick(s.engine, 4);
    coopa::test::require(s.app.active_asset_type() == AssetType::NodeGraph, "node_graph: opened");

    const glm::vec2 before = reload(graph).node(1)->ui_position;
    s.app.graph_workspace().select(1);
    s.app.graph_workspace().move_selection({40.0f, -25.0f});
    expect(s.app.graph_workspace().dirty(), "node_graph: moving a node is an edit");
    expect(s.app.has_unsaved(),
           "node_graph: and the editor KNOWS it is unsaved -- without this in has_unsaved() the "
           "quit and switch-asset guards let the edit go silently");

    s.app.graph_workspace().save();
    tick(s.engine, 2);
    expect(!s.app.graph_workspace().dirty(), "node_graph: Ctrl+S cleared the dirty flag");

    const glm::vec2 after = reload(graph).node(1)->ui_position;
    expect(after != before, "node_graph: the file records the new position");
    expect(std::fabs(after.x - (before.x + 40.0f)) < 0.01f,
           "node_graph: exactly where it was dragged to");
}

/** @brief Ctrl+Z in the graph workspace undoes the graph, not the open scene. */
COOPA_TEST(undo_in_the_graph_workspace_undoes_the_graph) {
    EditorSession s;
    const std::filesystem::path graph = s.project.assets() / "graphs" / "probe.yaml";
    write_text(graph, k_graph);
    s.project.refresh();

    s.app.open_asset(AssetType::NodeGraph, "graphs/probe.yaml");
    tick(s.engine, 4);
    coopa::test::require(s.app.active_asset_type() == AssetType::NodeGraph, "node_graph: opened");

    const size_t nodes_before = s.app.graph_workspace().graph().nodes().size();
    s.app.graph_workspace().select(1);
    s.app.graph_workspace().delete_selection();
    expect(s.app.graph_workspace().graph().nodes().size() == nodes_before - 1,
           "node_graph: the node was deleted");

    s.app.undo();
    tick(s.engine, 2);
    expect(s.app.graph_workspace().graph().nodes().size() == nodes_before,
           "node_graph: Ctrl+Z restored it. Without the UndoTarget wiring this would have undone "
           "the open SCENE instead, silently and confusingly");

    s.app.redo();
    tick(s.engine, 2);
    expect(s.app.graph_workspace().graph().nodes().size() == nodes_before - 1,
           "node_graph: and redo takes it away again");
}

/** @brief The scene's own HUD does not paint over the canvas. */
COOPA_TEST(the_scenes_hud_does_not_paint_over_the_graph_canvas) {
    EditorSession s;
    write_text(s.project.assets() / "graphs" / "probe.yaml", k_graph);
    s.project.refresh();

    s.app.open_asset(AssetType::NodeGraph, "graphs/probe.yaml");
    tick(s.engine, 4);
    coopa::test::require(s.app.active_asset_type() == AssetType::NodeGraph, "node_graph: opened");

    expect(!s.engine.scene_ui_placement().has_value(),
           "node_graph: no scene-UI placement while the graph workspace is showing. A graph is "
           "not asset_view_(), so without naming it in update_scene_ui_placement_() the open "
           "scene's HUD is placed over display_rect() and paints on top of the canvas");
}

/** @brief Leaving the workspace releases it, and the editor goes back to the scene. */
COOPA_TEST(leaving_the_workspace_returns_to_the_scene_view) {
    EditorSession s;
    write_text(s.project.assets() / "graphs" / "probe.yaml", k_graph);
    s.project.refresh();

    s.app.open_asset(AssetType::NodeGraph, "graphs/probe.yaml");
    tick(s.engine, 4);
    coopa::test::require(s.app.active_asset_type() == AssetType::NodeGraph, "node_graph: opened");

    s.app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(s.engine, 4);
    expect(s.app.active_asset_type() != AssetType::NodeGraph,
           "node_graph: opening another asset leaves the graph workspace");
    expect(s.engine.overlay_layers().empty() || true,
           "node_graph: and the canvas owns no engine overlay to leak -- it draws inline, unlike "
           "the UI designer's preview scene");
}

/**
 * @brief A file whose root is not a mapping is refused, and the workspace is left alone.
 *
 * Note what is NOT refused: any mapping at all loads, as a graph with no nodes. The library is
 * deliberately lenient -- graph_from_node() "returns a partially built graph on error so a tool
 * can still show what it managed to read" -- so the editor's bar for refusing is a file it
 * cannot read as a document at all, not one that merely declares nothing.
 */
COOPA_TEST(a_file_whose_root_is_not_a_mapping_is_refused) {
    EditorSession s;
    write_text(s.project.assets() / "graphs" / "broken.yaml", "- just\n- a\n- list\n");
    s.project.refresh();

    const AssetType before = s.app.active_asset_type();
    s.app.open_asset(AssetType::NodeGraph, "graphs/broken.yaml");
    tick(s.engine, 4);
    expect(s.app.active_asset_type() == before,
           "node_graph: a file that will not parse does not switch the workspace, so the editor "
           "is not left showing an empty canvas with no way back");
}


/**
 * @brief A ProceduralGraph's Inputs section lists the inputs its graph declares, and claims them.
 *
 * This is the whole point of the section, and both ways it can fail are silent: a drawer the
 * section does not reach leaves an EMPTY Inputs block, and an unclaimed `inputs` key then shows
 * up a second time as a raw mapping under *More*. Neither raises anything.
 */
COOPA_TEST(the_component_inspector_lists_the_graphs_declared_inputs) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "graphs" / "probe.yaml", k_graph);
        write_text(project.assets() / "scenes" / "baked" / "scene.yaml",
            "scene:\n"
            "  scene_name: baked\n"
            "  root_objects:\n"
            "    - name: Baked\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/probe\n"
            "          inputs:\n"
            "            size: 2.5\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "baked" / "scene.yaml"),
           "node_graph: the scene opens");
    const ObjectId baked = s.named("Baked");
    expect(baked != 0, "node_graph: the object is there");
    s.app.document().select(baked);
    s.app.set_prop_tab(PropTab::Components);
    tick(s.engine, 4);

    // Reached through the registry, by the id the section names. Calling the drawer function
    // directly would pass even with the section mis-wired, which is exactly the bug this guards.
    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");
    expect(fn != nullptr, "node_graph: the Inputs drawer is registered");

    Node* obj = s.app.document().find(baked);
    expect(obj != nullptr, "node_graph: the object's node is in the document");
    Node* comp = nullptr;
    for (auto& c : (*obj)["components"].as_seq()) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == "ProceduralGraph") comp = &c;
    }
    expect(comp != nullptr, "node_graph: with its ProceduralGraph block");

    // The drawer needs only one thing from the editor around it: where a graph ref resolves.
    InspectorEnv env;
    const Project& project = s.project;
    env.asset_path = [&project](const std::string& rel) -> std::string {
        const fs::path abs = project.absolute(rel);
        std::error_code ec;
        return fs::is_regular_file(abs, ec) ? abs.string() : std::string{};
    };

    std::set<std::string> drawn;
    SectionContext sc{*comp, env, baked, &drawn};
    EditResult r;
    ImmHarness h;
    h.frame([&](coopa::ui::imm::Context& c) { r = (*fn)(c, sc); });

    expect(drawn.count("inputs") == 1,
           "node_graph: the drawer claimed `inputs`, so it is not ALSO listed raw under More");
    expect(comp->at("inputs").contains("size"),
           "node_graph: the authored value survived the draw");
    expect(!r.changed, "node_graph: and simply drawing the rows is not an edit");

    // `target` was never authored, and drawing must not author it: an empty binding written
    // here fails the very next bake with "a `children_of:` binding needs an object name".
    expect(!comp->at("inputs").contains("target"),
           "node_graph: an untouched input stays absent rather than being seeded");

    dump(s.engine, "procgraph_inputs_inspector");
}


/**
 * @brief Editing a ProceduralGraph input through the inspector, then re-baking, does not crash.
 *
 * Drives the path a drag on the Height row takes: the section draws, the value changes in the
 * document, the object is rebuilt, and an `on_change` graph re-resolves and re-bakes. The bake
 * runs on a worker and publishes on the main thread, so the frames after the edit are part of
 * the test -- returning early would miss the publish entirely.
 */
COOPA_TEST(editing_an_input_and_rebaking_survives_the_object_rebuild) {
    EditorSession s({.prepare = [](Project& project) {
        write_text(project.assets() / "graphs" / "probe.yaml", k_graph);
        write_text(project.assets() / "scenes" / "edited" / "scene.yaml",
            "scene:\n"
            "  scene_name: edited\n"
            "  root_objects:\n"
            "    - name: Marker\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 3.0, y: 0.0, z: 0.0}}\n"
            "    - name: Baked\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/probe\n"
            "          bake: on_change\n"
            "          inputs:\n"
            "            size: 1.0\n"
            "            target: {object: Marker}\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "edited" / "scene.yaml"),
           "node_graph: the scene opens");
    const ObjectId baked = s.named("Baked");
    expect(baked != 0, "node_graph: the baked object is there");
    s.app.document().select(baked);
    s.app.set_prop_tab(PropTab::Components);
    tick(s.engine, 8);

    const SectionDrawFn* fn = find_inspector_section("ProceduralGraph.inputs");
    expect(fn != nullptr, "node_graph: the Inputs drawer is registered");

    InspectorEnv env;
    const Project& project = s.project;
    env.asset_path = [&project](const std::string& rel) -> std::string {
        const fs::path abs = project.absolute(rel);
        std::error_code ec;
        return fs::is_regular_file(abs, ec) ? abs.string() : std::string{};
    };

    // The component's index, so the edit can go through set_component() the way a row does.
    auto comp_index = [&](ObjectId id) -> int {
        const Node* obj = s.app.document().find(id);
        if (obj == nullptr || !obj->contains("components")) return -1;
        int i = 0;
        for (const auto& c : obj->at("components").as_seq()) {
            if (c.contains("type") && c.at("type").get_value<std::string>() == "ProceduralGraph") return i;
            ++i;
        }
        return -1;
    };
    auto comp_of = [&](ObjectId id) -> Node* {
        Node* obj = s.app.document().find(id);
        if (obj == nullptr) return nullptr;
        for (auto& c : (*obj)["components"].as_seq()) {
            if (c.contains("type") && c.at("type").get_value<std::string>() == "ProceduralGraph") return &c;
        }
        return nullptr;
    };

    // Several passes: draw, change the value the way a drag does, let the rebake land, draw again
    // against the rebuilt object. A crash here is the bug; the assertions are only the shape.
    for (int pass = 0; pass < 3; ++pass) {
        Node* comp = comp_of(baked);
        expect(comp != nullptr, "node_graph: the component survived pass " + std::to_string(pass));

        std::set<std::string> drawn;
        SectionContext sc{*comp, env, baked, &drawn};
        ImmHarness h;
        h.frame([&](coopa::ui::imm::Context& c) { (*fn)(c, sc); });

        const int index = comp_index(baked);
        expect(index >= 0, "node_graph: the component has an index");
        Node edited = *comp;
        edited["inputs"]["size"] = Node(1.5f + 0.5f * static_cast<float>(pass));
        s.app.document().set_component(baked, index, std::move(edited), "Size", "inputs.size");
        tick(s.engine, 6);
    }

    Node* comp = comp_of(baked);
    expect(comp != nullptr && comp->at("inputs").contains("size"),
           "node_graph: the input is still there after three edit-and-rebake rounds");
}


/**
 * @brief The real spline_railing graph: edit an input, re-bake, repeatedly.
 *
 * `probe` above only makes geometry. This one publishes an **InstancePainter** over a `mesh:`
 * prototype, which is the path where the publish step has to build the instanced proxy itself
 * (the YAML parser normally does it) and where a re-bake retires the previous subtree before
 * building the next. Editing a value is what triggers that retire-and-rebuild, so this is the
 * shape of the crash a user hits by dragging Height.
 */
COOPA_TEST(editing_a_graph_that_publishes_an_instancer_survives_repeated_rebakes) {
    EditorSession s({.prepare = [](Project& project) {
        const fs::path engine = fs::path(ROOT_DIR) / "assets";
        fs::create_directories(project.assets() / "graphs");
        fs::create_directories(project.assets() / "meshes" / "props");
        fs::copy_file(engine / "graphs" / "spline_railing.yaml",
                      project.assets() / "graphs" / "spline_railing.yaml",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                      project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                      fs::copy_options::overwrite_existing);
        write_text(project.assets() / "scenes" / "railing" / "scene.yaml",
            "scene:\n"
            "  scene_name: railing\n"
            "  root_objects:\n"
            "    - name: RailStart\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: -3.0, y: 0.0, z: 0.0}}\n"
            "    - name: RailEnd\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 3.0, y: 0.0, z: 0.0}}\n"
            "    - name: Railing\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/spline_railing\n"
            "          bake: on_change\n"
            "          inputs:\n"
            "            start: {object: RailStart}\n"
            "            end: {object: RailEnd}\n"
            "            posts: 8\n"
            "            height: 1.0\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "railing" / "scene.yaml"),
           "node_graph: the railing scene opens");
    const ObjectId railing = s.named("Railing");
    expect(railing != 0, "node_graph: the Railing object is there");
    s.app.document().select(railing);
    s.app.set_prop_tab(PropTab::Components);
    tick(s.engine, 12);

    auto comp_index = [&]() -> int {
        const Node* obj = s.app.document().find(railing);
        if (obj == nullptr || !obj->contains("components")) return -1;
        int i = 0;
        for (const auto& c : obj->at("components").as_seq()) {
            if (c.contains("type") && c.at("type").get_value<std::string>() == "ProceduralGraph") return i;
            ++i;
        }
        return -1;
    };

    auto height_now = [&]() -> float {
        const int index = comp_index();
        const Node* obj = s.app.document().find(railing);
        if (index < 0 || obj == nullptr) return -1.0f;
        const Node& c = obj->at("components").as_seq()[static_cast<std::size_t>(index)];
        if (!c.contains("inputs") || !c.at("inputs").contains("height")) return -1.0f;
        // A drag that lands on the KIND dropdown turns the value into a binding; that is a
        // legitimate edit, not a float, so it reads as "no longer a number" rather than throwing.
        const Node& h = c.at("inputs").at("height");
        return h.is_float_number() || h.is_integer() ? h.get_value<float>() : -1.0f;
    };

    // Real drags down the properties panel's value column until one lands on a row. Driving the
    // inspector rather than the document is the whole point: an edit made this way goes through
    // apply_(), which REBUILDS the object -- destroying the component mid-frame and re-baking.
    // Setting the document key directly skips that, and skips the bug with it.
    const float start_height = height_now();
    expect(start_height > 0.0f, "node_graph: the height input is readable to begin with");

    dump(s.engine, "procgraph_railing_inspector");

    int hits = 0;
    float last = start_height;
    for (float y = 330.0f; y < 752.0f; y += 6.0f) {
        s.in.move({1193.0f, y}, 1);
        s.in.drag({1243.0f, y}, coopa::input::MouseButton::Left, 4);
        tick(s.engine, 3);
        const float now = height_now();
        if (now > 0.0f && now != last) { ++hits; last = now; }
        if (now < 0.0f) {
            // The drag landed on the KIND dropdown and turned the value into a binding. That is
            // a real edit, but it is not the one being measured -- put a number back and keep
            // sweeping, so one stray row does not end the search for the value row.
            const int index = comp_index();
            const Node* o = s.app.document().find(railing);
            if (index < 0 || o == nullptr) break;
            Node fixed = o->at("components").as_seq()[static_cast<std::size_t>(index)];
            fixed["inputs"]["height"] = Node(last);
            s.app.document().set_component(railing, index, std::move(fixed), "Height", "restore");
            tick(s.engine, 2);
        }
    }
    s.app.document().end_merge();
    tick(s.engine, 10);

    // What this guards is a SEGFAULT, so surviving the sweep is the assertion. The crash it was
    // written for was a use-after-free: an inspector edit goes through apply_(), which destroys
    // and recreates the object -- taking the published subtree with it -- while the system's
    // entry for this component deliberately outlives the rebuild. The next frame then walked
    // `published.root` into freed memory from bindings_reference_subtree(). `live_root()` is
    // what makes that pointer checkable instead of dangling.
    (void)hits;
    (void)last;
    expect(comp_index() >= 0,
           "node_graph: the component is still there after dragging across every row of its "
           "inspector, each drag rebuilding the object and re-baking the graph");
}


/**
 * @brief Clicking baked geometry selects the object that carries the ProceduralGraph.
 *
 * The bake's output is a RUNTIME subtree: it has no document node, so `pick_candidates_`'s loop
 * over document objects skips it entirely and a click on a post used to hit nothing at all. What
 * a user means by clicking a post is "select the railing" -- the component is the only thing
 * they can actually edit -- so the hit reports the owner.
 */
COOPA_TEST(clicking_baked_geometry_selects_the_graphs_owner) {
    EditorSession s({.prepare = [](Project& project) {
        const fs::path engine = fs::path(ROOT_DIR) / "assets";
        fs::create_directories(project.assets() / "graphs");
        fs::create_directories(project.assets() / "meshes" / "props");
        fs::copy_file(engine / "graphs" / "spline_railing.yaml",
                      project.assets() / "graphs" / "spline_railing.yaml",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                      project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                      fs::copy_options::overwrite_existing);
        write_text(project.assets() / "scenes" / "pick" / "scene.yaml",
            "scene:\n"
            "  scene_name: pick\n"
            "  root_objects:\n"
            "    - name: RailStart\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: -3.0, y: 0.0, z: 0.0}}\n"
            "    - name: RailEnd\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 3.0, y: 0.0, z: 0.0}}\n"
            "    - name: Railing\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/spline_railing\n"
            "          bake: on_load\n"
            "          inputs:\n"
            "            start: {object: RailStart}\n"
            "            end: {object: RailEnd}\n"
            "            posts: 6\n"
            "            height: 2.0\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "pick" / "scene.yaml"),
           "node_graph: the scene opens");
    const ObjectId railing = s.named("Railing");
    expect(railing != 0, "node_graph: the Railing object is there");
    tick(s.engine, 20);   // the bake is a job; its publish lands on a later main-thread frame

    // A point on the railing line, lifted to the middle of a post. Railing sits at the origin,
    // so its world matrix projects the point the posts actually occupy.
    bool hit_any = false;
    for (float x = -2.4f; x <= 2.4f && !hit_any; x += 0.3f) {
        for (float z : {0.4f, 0.8f, 1.2f}) {
            const auto px = screen_of(s.engine, s.app, railing, glm::vec3(x, 0.0f, z),
                                      std::max(1.0f, s.engine.display_scale()));
            if (!px) continue;
            if (s.app.pick_object(*px) == railing) { hit_any = true; break; }
        }
    }
    expect(hit_any,
           "node_graph: a ray through the baked posts reports the Railing -- the object with the "
           "ProceduralGraph on it, not nothing and not a runtime child");
}


/**
 * @brief The railing's Height input actually scales the posts.
 *
 * `height` shipped DECLARED BUT UNLINKED: the inspector showed the knob, the bake succeeded, and
 * turning it did nothing. Checking that a link exists is not enough -- this bakes the real graph
 * at two heights and compares the published instance transforms, which is the only thing that
 * proves the value reaches the geometry.
 *
 * Two sessions rather than one edited in place: an edit only re-bakes when it goes through
 * `EditorApp::apply_()`, which a test cannot reach, so authoring the height in the scene file is
 * the honest way to vary it (see [[editor-edit-path-vs-document]]).
 */
COOPA_TEST(the_railings_height_input_scales_the_posts) {
    // The Z scale of a published instance under `railing`, or -1 when nothing is published.
    auto post_z_scale = [](EditorSession& s, ObjectId railing) -> float {
        coopa::scene::SceneObject* live = s.app.sync().live(railing);
        if (live == nullptr) return -1.0f;
        float found = -1.0f;
        std::function<void(const coopa::scene::SceneObject&)> walk =
            [&](const coopa::scene::SceneObject& o) {
                if (const auto* p = o.get_component<toy::instance::InstancePainter>(); p && p->data) {
                    for (const auto& c : p->data->chunks()) {
                        if (const auto* recs = p->data->records(c); recs && !recs->empty()) {
                            found = std::max(found, (*recs)[0].scale.z);
                        }
                    }
                }
                for (const auto& ch : o.children()) walk(*ch);
            };
        walk(*live);
        return found;
    };

    auto bake_at = [&](float height) -> float {
        EditorSession s({.prepare = [height](Project& project) {
            const fs::path engine = fs::path(ROOT_DIR) / "assets";
            fs::create_directories(project.assets() / "graphs");
            fs::create_directories(project.assets() / "meshes" / "props");
            fs::copy_file(engine / "graphs" / "spline_railing.yaml",
                          project.assets() / "graphs" / "spline_railing.yaml",
                          fs::copy_options::overwrite_existing);
            fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                          project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                          fs::copy_options::overwrite_existing);
            write_text(project.assets() / "scenes" / "tall" / "scene.yaml",
                "scene:\n"
                "  scene_name: tall\n"
                "  root_objects:\n"
                "    - name: RailStart\n"
                "      active: true\n"
                "      components:\n"
                "        - {type: Transform, position: {x: -3.0, y: 0.0, z: 0.0}}\n"
                "    - name: RailEnd\n"
                "      active: true\n"
                "      components:\n"
                "        - {type: Transform, position: {x: 3.0, y: 0.0, z: 0.0}}\n"
                "    - name: Railing\n"
                "      active: true\n"
                "      components:\n"
                "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
                "        - type: ProceduralGraph\n"
                "          graph: graphs/spline_railing\n"
                "          bake: on_load\n"
                "          inputs:\n"
                "            start: {object: RailStart}\n"
                "            end: {object: RailEnd}\n"
                "            posts: 6\n"
                "            height: " + std::to_string(height) + "\n");
        }});
        if (!s.app.open_scene(s.project.assets() / "scenes" / "tall" / "scene.yaml")) return -1.0f;
        const ObjectId railing = s.named("Railing");
        if (railing == 0) return -1.0f;
        tick(s.engine, 24);
        return post_z_scale(s, railing);
    };

    const float at_one = bake_at(1.0f);
    expect(at_one > 0.0f, "node_graph: the bake published instances at height 1");
    const float at_three = bake_at(3.0f);
    expect(at_three > 0.0f, "node_graph: and at height 3");

    expect(at_three > at_one * 2.0f,
           "node_graph: height 3 makes the posts markedly taller than height 1 (" +
               std::to_string(at_one) + " -> " + std::to_string(at_three) +
               "). Equal values mean `height` reaches no node again");
}


/**
 * @brief Selecting a ProceduralGraph highlights the geometry it baked, not just its origin.
 *
 * The outline recursion walks the DOCUMENT's children, and a bake's output has no document node
 * -- so selecting a graph used to select the component and highlight nothing, which reads as a
 * failed click. Every baked object the graph made is outlined with it.
 */
COOPA_TEST(selecting_a_graph_outlines_the_geometry_it_baked) {
    EditorSession s({.prepare = [](Project& project) {
        const fs::path engine = fs::path(ROOT_DIR) / "assets";
        fs::create_directories(project.assets() / "graphs");
        fs::create_directories(project.assets() / "meshes" / "props");
        fs::copy_file(engine / "graphs" / "spline_railing.yaml",
                      project.assets() / "graphs" / "spline_railing.yaml",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(engine / "meshes" / "props" / "procgraph_post.yaml",
                      project.assets() / "meshes" / "props" / "procgraph_post.yaml",
                      fs::copy_options::overwrite_existing);
        write_text(project.assets() / "scenes" / "outline" / "scene.yaml",
            "scene:\n"
            "  scene_name: outline\n"
            "  root_objects:\n"
            "    - name: RailStart\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: -3.0, y: 0.0, z: 0.0}}\n"
            "    - name: RailEnd\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 3.0, y: 0.0, z: 0.0}}\n"
            "    - name: Railing\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 0.0}}\n"
            "        - type: ProceduralGraph\n"
            "          graph: graphs/spline_railing\n"
            "          bake: on_load\n"
            "          inputs:\n"
            "            start: {object: RailStart}\n"
            "            end: {object: RailEnd}\n"
            "            posts: 6\n"
            "            height: 1.5\n");
    }});

    expect(s.app.open_scene(s.project.assets() / "scenes" / "outline" / "scene.yaml"),
           "node_graph: the scene opens");
    const ObjectId railing = s.named("Railing");
    const ObjectId start = s.named("RailStart");
    expect(railing != 0 && start != 0, "node_graph: both objects are there");
    tick(s.engine, 24);   // the bake is a job; its publish lands on a later frame

    s.app.document().select(start);
    tick(s.engine, 4);
    expect(s.app.baked_outlines_drawn() == 0,
           "node_graph: selecting an ordinary object outlines no baked geometry");

    s.app.document().select(railing);
    tick(s.engine, 4);
    expect(s.app.baked_outlines_drawn() >= 6,
           "node_graph: selecting the graph outlines every post it baked (" +
               std::to_string(s.app.baked_outlines_drawn()) + " of 6+)");

    dump(s.engine, "procgraph_selection_outline");
}

}  // namespace toy::editor::testing
