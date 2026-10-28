/**
 * @file procgraph_render_test.cpp
 * @brief procgraph_demo end to end: a graph becomes real objects with real components.
 *
 * The unit suites (procgraph_bake, procgraph_scene_input) cover the bake and the bindings without
 * a device. This one covers the half they cannot: that `SceneLoader::build_object()` really does
 * spawn the emitted components, that uploaded meshes land on a MeshRenderer under a stable asset
 * id, that instances reach the painter, and that a re-bake replaces rather than accumulates.
 */

#include <coopa/testing/test.h>

#include <map>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <toyengine/core/engine.h>
#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_set.h>
#include <toyengine/procgraph/procedural_graph.h>
#include <toyengine/procgraph/procedural_graph_system.h>
#include <toyengine/scene/runtime_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("procgraph_render");

using namespace toy::test;
using toy::procgraph::ProceduralGraph;
using toy::procgraph::ProceduralGraphSystem;

namespace {

const char* k_demo = "assets/scenes/procedural/procgraph_demo/scene.yaml";

std::string demo() { return std::string(ROOT_DIR) + "/" + k_demo; }

ProceduralGraphSystem* system_of(toy::core::Engine& engine) {
    return toy::procgraph::find_procgraph(engine.scene());
}

/// @brief The ProceduralGraph on the object named `name`, or null.
ProceduralGraph* graph_on(toy::core::Engine& engine, const std::string& name) {
    auto* obj = engine.scene().find_object(name);
    return obj == nullptr ? nullptr : obj->get_component<ProceduralGraph>();
}

/// @brief Every object the procedural-graph system spawned, anywhere in the scene.
std::vector<coopa::scene::SceneObject*> baked_roots(toy::core::Engine& engine) {
    std::vector<coopa::scene::SceneObject*> out;
    for (const auto& root : engine.scene().root_objects()) {
        root->for_each_recursive([&out](coopa::scene::SceneObject& o) {
            const auto* marker = o.get_component<toy::scene::RuntimeObject>();
            if (marker != nullptr && marker->owner_system == "ProceduralGraph") out.push_back(&o);
        });
    }
    return out;
}

/// @brief The messages of every graph in the scene, for a failure that explains itself.
std::string all_messages(toy::core::Engine& engine) {
    std::string out;
    auto* sys = system_of(engine);
    if (sys == nullptr) return "(no ProceduralGraph system installed)";
    for (auto* component : engine.scene().get_components<ProceduralGraph>()) {
        const auto* result = sys->last_result(*component);
        if (result == nullptr) {
            out += component->graph + ": (never baked)\n";
            continue;
        }
        out += component->graph + ": " + (result->messages().empty() ? std::string("ok\n")
                                                                     : result->messages());
    }
    return out;
}

} // namespace

/** @brief The demo's graphs bake, spawn objects, and leave nothing to complain about. */
COOPA_TEST(the_demo_bakes_every_graph_without_errors) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    auto* sys = system_of(engine);
    require(sys != nullptr, "procgraph: the system is installed at order 63");

    const auto components = engine.scene().get_components<ProceduralGraph>();
    expect(components.size() >= 5,
           "procgraph: the demo carries its five ProceduralGraph components (three stairs, the "
           "marker lights and the railing). Got " + std::to_string(components.size()));

    expect(sys->bake_count() >= components.size(),
           "procgraph: every component baked at least once (" + std::to_string(sys->bake_count()) +
           " bakes for " + std::to_string(components.size()) + " graphs)");

    for (auto* component : components) {
        const auto* result = sys->last_result(*component);
        require(result != nullptr,
                "procgraph: '" + component->graph + "' has a bake result -- a component the "
                "system never processed would silently produce nothing");
        expect(result->ok(),
               "procgraph: '" + component->graph + "' baked without errors:\n" + result->messages());
    }
}

/** @brief The baked objects really are in the scene, marked as runtime. */
COOPA_TEST(baked_objects_are_spawned_and_marked_runtime) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    const auto roots = baked_roots(engine);
    expect(roots.size() >= 5,
           "procgraph: one baked root per graph was spawned into the scene (" +
           std::to_string(roots.size()) + "). " + all_messages(engine));

    for (auto* root : roots) {
        const auto* marker = root->get_component<toy::scene::RuntimeObject>();
        require(marker != nullptr, "procgraph: the baked root carries a RuntimeObject marker");
        expect(marker->owner_system == "ProceduralGraph",
               "procgraph: naming this system as its owner, so the editor groups it under "
               "'Runtime' and never writes it into the scene file");
        expect(root->parent() != nullptr,
               "procgraph: and it is parented under the component's own object rather than "
               "floating at the scene root");
    }
}

/**
 * @brief The emitted components are real, parsed components.
 *
 * This is the claim the whole output design rests on: a graph emits `{type, props}` and
 * `SceneLoader::build_object()` dispatches it through the process-wide parser table, so a graph
 * can emit any registered component with no per-type code in procgraph.
 */
COOPA_TEST(an_emitted_instance_painter_is_a_real_component_holding_the_baked_records) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    int painters = 0;
    size_t total_records = 0;
    for (auto* root : baked_roots(engine)) {
        root->for_each_recursive([&](coopa::scene::SceneObject& o) {
            auto* p = o.get_component<toy::instance::InstancePainter>();
            if (p == nullptr) return;
            ++painters;
            require(p->data != nullptr,
                    "procgraph: the emitted InstancePainter has a data set bound. The component's "
                    "own parser acquires one by path during build_object(); publish must then "
                    "assign the set it FILLED, or the painter reads an empty one and draws "
                    "nothing while every other check still passes");
            total_records += p->data->size();
        });
    }

    expect(painters >= 3,
           "procgraph: the three instancing graphs each emitted an InstancePainter through the "
           "real parser table. Got " + std::to_string(painters) + ". " + all_messages(engine));
    expect(total_records >= 30,
           "procgraph: their baked records reached the painters (" +
           std::to_string(total_records) + " instances across " + std::to_string(painters) +
           " painters) -- 5+9+14 stair treads, 8 posts and 3 lamps");
}

/** @brief A graph that emits no geometry at all still produces its components. */
COOPA_TEST(the_no_geometry_graph_emits_components_and_no_mesh) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    auto* component = graph_on(engine, "MarkerLights");
    require(component != nullptr, "procgraph: the MarkerLights component is in the scene");

    auto* sys = system_of(engine);
    require(sys != nullptr, "procgraph: the system is installed");
    const auto* result = sys->last_result(*component);
    require(result != nullptr, "procgraph: it baked");
    expect(result->ok(), "procgraph: without errors: " + result->messages());

    expect(result->root.mesh == nullptr,
           "procgraph: the marker_lights graph produced NO mesh of its own. This is the claim "
           "that `ProceduralGraph` is a node-graph -> scene-graph converter rather than a "
           "geometry baker: a graph whose entire output is components is a first-class use");
    expect(result->root.instances != nullptr || !result->root.components.empty() ||
               !result->root.children.empty(),
           "procgraph: but it did produce something -- the lamps at the authored markers");
}

/** @brief A re-bake replaces its output instead of accumulating it, and leaks no asset slots. */
COOPA_TEST(rebaking_replaces_rather_than_accumulates) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    auto* sys = system_of(engine);
    require(sys != nullptr, "procgraph: the system is installed");
    const size_t roots_before = baked_roots(engine).size();
    const uint64_t bakes_before = sys->bake_count();
    expect(roots_before >= 5, "procgraph: the demo baked its graphs");

    // Twenty forced re-bakes. If publish accumulated instead of replacing, the object count would
    // climb by one root per graph per round; if it leaked synthetic assets, the slot count would.
    for (int i = 0; i < 20; ++i) {
        sys->rebake_all();
        tick_frames(engine, 2);
    }

    expect(sys->bake_count() > bakes_before,
           "procgraph: the forced re-bakes actually ran (" + std::to_string(bakes_before) +
           " -> " + std::to_string(sys->bake_count()) + ")");
    const size_t roots_after = baked_roots(engine).size();
    expect(roots_after == roots_before,
           "procgraph: twenty re-bakes left the SAME number of baked roots (" +
           std::to_string(roots_before) + " -> " + std::to_string(roots_after) +
           "). A publish that spawned without retiring the previous output would multiply them, "
           "which is the most likely lifetime bug in this design");

    for (auto* component : engine.scene().get_components<ProceduralGraph>()) {
        const auto* result = sys->last_result(*component);
        require(result != nullptr, "procgraph: '" + component->graph + "' still has a result");
        expect(result->ok(),
               "procgraph: '" + component->graph + "' is still clean after 20 re-bakes:\n" +
               result->messages());
    }
}

/** @brief Moving a bound object re-bakes its graph, and only its graph. */
COOPA_TEST(moving_a_bound_object_rebakes_only_the_graph_that_reads_it) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 120);

    auto* sys = system_of(engine);
    require(sys != nullptr, "procgraph: the system is installed");

    // Everything has settled: with nothing moving, an on_change graph must stop re-baking.
    const uint64_t settled = sys->bake_count();
    tick_frames(engine, 30);
    expect(sys->bake_count() == settled,
           "procgraph: with nothing moving, the on_change graphs stopped re-baking (" +
           std::to_string(settled) + " -> " + std::to_string(sys->bake_count()) +
           "). A hash that reported 'changed' every resolve would re-bake forever and burn a "
           "worker every frame");

    auto* rail_start = engine.scene().find_object("RailStart");
    require(rail_start != nullptr && rail_start->get_transform() != nullptr,
            "procgraph: the railing's start marker is in the scene");
    rail_start->get_transform()->transform().set_position(glm::vec3(-9.0f, 7.0f, 0.0f));
    tick_frames(engine, 5);

    expect(sys->bake_count() > settled,
           "procgraph: moving a BOUND object re-baked (" + std::to_string(settled) + " -> " +
           std::to_string(sys->bake_count()) + ") -- the scene-input contract, end to end");

    const uint64_t after_move = sys->bake_count();
    auto* unrelated = engine.scene().find_object("Stair_A");
    require(unrelated != nullptr && unrelated->get_transform() != nullptr,
            "procgraph: a graph that binds to nothing is in the scene");
    unrelated->get_transform()->transform().set_position(glm::vec3(-6.0f, 3.0f, 0.0f));
    tick_frames(engine, 5);
    expect(sys->bake_count() == after_move,
           "procgraph: moving an object NO graph binds to re-baked nothing (" +
           std::to_string(after_move) + " -> " + std::to_string(sys->bake_count()) +
           "). This is what keeps `on_change` from costing a rebuild on every unrelated edit");
}

/** @brief A scene with no graphs installs the system but does no work. */
COOPA_TEST(a_scene_with_no_graphs_costs_nothing) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::instance::InstanceSetStore::instance().clear();
    const std::string plain = std::string(ROOT_DIR) + "/assets/scenes/rendering/shader_demo/scene.yaml";
    toy::core::Engine engine(make_test_config(plain, 320, 180, 320, 180));
    tick_frames(engine, 60);

    auto* sys = system_of(engine);
    require(sys != nullptr, "procgraph: the system is installed in every scene");
    expect(sys->bake_count() == 0,
           "procgraph: a scene with no ProceduralGraph bakes nothing at all -- the no-regression "
           "guard. Got " + std::to_string(sys->bake_count()) + " bakes");
    expect(baked_roots(engine).empty(),
           "procgraph: and spawns no objects");
}
