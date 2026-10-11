/**
 * @file particle_panel_test.cpp
 * @brief The ParticleSystem panel over a live scene: a grass scatter under a Heightmap Terrain shows
 *        its Terrain Placement section and streams instances, a higher `terrain.density` grows
 *        more of them, and an emitter shows the emitter sections. Screenshots (EDITOR_DUMP_DIR)
 *        record both layouts.
 */

#include <coopa/testing/test.h>

#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/terrain_scatter.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("particle_panel");

namespace toy::editor::testing {

namespace {

/** @brief Replaces object `id`'s ParticleSystem block and rebuilds it live. */
void set_particles(EditorSession& s, ObjectId id, const std::function<void(Node&)>& fn) {
    auto& doc = s.app.document();
    const int ci = doc.find_component(id, "ParticleSystem");
    Node comp = doc.find(id)->at("components").as_seq()[static_cast<std::size_t>(ci)];
    fn(comp);
    doc.set_component(id, ci, comp, "Edit particles");
    s.app.sync().apply(s.engine, doc, {ChangeScope::Object, id});
}

std::size_t live_instances(EditorSession& s, ObjectId id) {
    auto* ps = s.app.sync().live(id)->get_component<toy::particles::ParticleSystem>();
    return ps && ps->terrain_scatter_if() ? ps->terrain_scatter_if()->instance_count() : 0;
}

} // namespace

COOPA_TEST(terrain_scatter_density_drives_the_grass) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;

    const ObjectId terrain = app.create_with_component("HeightmapTerrain", "Terrain");
    tick(engine, 4);
    const ObjectId grass = app.create_with_component("ParticleSystem", "Grass", terrain);
    tick(engine, 2);
    set_particles(session, grass, [](Node& c) {
        c["mode"] = Node(std::string("scatter"));
        c["shape"] = Node(std::string("terrain"));
        Node t = Node::mapping();
        t["density"] = Node(0.05);
        c["terrain"] = t;
        c["render_mode"] = Node(std::string("mesh"));
        c["render_mesh"] = Node(std::string("cube"));
        c["start_size"] = Node::sequence();
        c["start_size"].as_seq().push_back(Node(0.3));
        c["start_size"].as_seq().push_back(Node(0.5));
        c["max_draw_distance"] = Node(2000.0);   // 0 would be automatic (~75 m for a 0.5 m cube)
    });
    app.document().clear_selection();
    app.document().select(terrain);
    app.frame_selected();
    app.document().clear_selection();
    app.document().select(grass);
    app.set_prop_tab(PropTab::Components);
    tick(engine, 60);   // a few chunks a frame
    const std::size_t sparse = live_instances(session, grass);
    expect(sparse > 0, "the scatter grows over the terrain (" + std::to_string(sparse) + ")");
    dump(engine, "particle_panel_terrain");

    set_particles(session, grass, [](Node& c) { c["terrain"]["density"] = Node(0.2); });
    tick(engine, 60);
    const std::size_t dense = live_instances(session, grass);
    expect(dense > sparse * 3, "four times the density grows about four times the instances (" + std::to_string(sparse) + " -> " +
                                   std::to_string(dense) + ")");

    // An emitter: the emitter sections instead.
    const ObjectId fx = app.create_with_component("ParticleSystem", "Sparks");
    tick(engine, 4);
    app.document().clear_selection();
    app.document().select(fx);
    app.set_prop_tab(PropTab::Components);
    tick(engine, 10);
    dump(engine, "particle_panel_emitter");
}

} // namespace toy::editor::testing
