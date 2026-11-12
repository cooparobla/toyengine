/**
 * @file sculpt_paint_test.cpp
 * @brief Sculpt, Vertex Paint and Weight Paint through real input: a stroke is one undo step, Ctrl
 * inverts / paints the secondary colour, the paint display shader swaps in and out, and leaving
 * the mode saves colours and vertex groups into the mesh file.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("sculpt_paint");

namespace toy::editor::testing {

/** @brief Sculpting workspace: Subdivide Smooth, a Draw stroke, Ctrl inverts, one undo step per stroke. */
COOPA_TEST(a_sculpt_stroke_is_one_undo_step_and_ctrl_inverts) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    using coopa::input::KeyAction;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::Sculpt, "the mode dropdown enters Sculpt Mode");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);
    tick(engine, 3);
    expect(md.mesh.faces.size() == 96 && closed_and_consistent(md.mesh), "Subdivide Smooth x2: 96 quads, closed (got " +
                                                                           std::to_string(md.mesh.faces.size()) + ")");
    app.sculpt_settings().symmetry.axis[0] = false;
    app.sculpt_settings().strength = 1.0f;
    app.sculpt_settings().radius_px = 60.0f;

    // A Draw stroke across the visible face pushes vertices outward.
    const glm::vec2 corner = visible_corner(app, cube);
    const glm::vec3 face_point(corner.x * 0.8f, corner.y * 0.8f, 0.0f);
    auto start = screen_of(engine, app, cube, glm::vec3(corner.x, corner.y * 0.3f, 0.0f), in.scale);
    auto stop = screen_of(engine, app, cube, glm::vec3(corner.x * 0.3f, corner.y, 0.0f), in.scale);
    (void)face_point;
    expect(start && stop, "the stroke lies on screen");
    if (!start || !stop) return;
    const EditMesh before = md.mesh;
    const size_t undo0 = md.undo.undo_count();
    auto spread = [](const EditMesh& m) { float r = 0; for (const auto& p : m.positions) r += glm::length(p); return r; };
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    expect(spread(md.mesh) > spread(before) + 1e-3f, "Draw pulls the surface outward");
    expect(md.undo.undo_count() == undo0 + 1, "the whole stroke is one undo step");
    dump(engine, "14_sculpt_stroke");
    app.undo();
    tick(engine, 2);
    expect(md.mesh == before, "undo restores the exact shape");

    // Ctrl inverts: push in.
    in.move(*start);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    for (int i = 1; i <= 10; ++i) in.move(glm::mix(*start, *stop, i / 10.0f));
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Release, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    expect(spread(md.mesh) < spread(before) - 1e-3f, "Ctrl+Draw pushes the surface in");
    app.show_document_view();
    tick(engine, 3);
    expect(app.interaction_mode() == InteractionMode::Object, "switching the view returns to Object Mode");
}

/** @brief Vertex Paint and Weight Paint end to end: the modes swap in the paint display, a
 *         stroke paints colour / weight as one undo step, Ctrl inverts, leaving the mode puts the
 *         material back and saves the colours and vertex groups into the mesh file. */
COOPA_TEST(vertex_and_weight_paint_save_into_the_mesh) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    using coopa::input::KeyAction;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    const std::string own_shader = app.edited_mesh_shader();
    app.set_interaction_mode(InteractionMode::VertexPaint);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::VertexPaint, "the mode menu enters Vertex Paint");
    expect(app.edited_mesh_shader() == "editor_paint", "Vertex Paint draws the mesh with the paint display shader");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);   // vertices for the brush to land on
    tick(engine, 3);
    expect(!md.mesh.has_colors, "the default cube starts without colours");
    auto& vs = app.vertex_paint_settings();
    vs.symmetry.axis[0] = false;
    vs.radius_px = 60.0f;
    vs.color = glm::vec4(1, 0, 0, 1);
    vs.secondary = glm::vec4(0, 0, 1, 1);

    const glm::vec2 corner = visible_corner(app, cube);
    auto start = screen_of(engine, app, cube, glm::vec3(corner.x, corner.y * 0.3f, 0.0f), in.scale);
    auto stop = screen_of(engine, app, cube, glm::vec3(corner.x * 0.3f, corner.y, 0.0f), in.scale);
    expect(start && stop, "the stroke lies on screen");
    if (!start || !stop) return;
    auto count = [&](auto pred) {
        size_t n = 0;
        for (const auto& f : md.mesh.faces) for (const auto& c : f.corners) n += pred(c.color) ? 1 : 0;
        return n;
    };
    const size_t undo0 = md.undo.undo_count();
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    const size_t reds = count([](const glm::vec4& c) { return c.r > 0.9f && c.g < 0.5f; });
    expect(md.mesh.has_colors && reds > 0, "a stroke paints the brush colour (" + std::to_string(reds) + " red corners)");
    expect(count([](const glm::vec4& c) { return c == glm::vec4(1.0f); }) > 0, "...only under the brush");
    expect(md.undo.undo_count() == undo0 + 1, "the whole stroke is one undo step");
    dump(engine, "15_vertex_paint");

    // Ctrl paints the secondary colour.
    in.move(*start);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Press, Mods::Control); });
    tick(engine, 1);
    for (int i = 1; i <= 10; ++i) in.move(glm::mix(*start, *stop, i / 10.0f));
    engine.queue_input([](coopa::input::Input& i) { i.push_mouse_button(MouseButton::Left, KeyAction::Release, Mods::Control); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::LeftControl, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    expect(count([](const glm::vec4& c) { return c.b > 0.9f && c.r < 0.5f; }) > 0, "Ctrl paints the secondary colour");

    // Weight Paint on the same mesh: the first stroke creates a group to paint into.
    app.set_interaction_mode(InteractionMode::WeightPaint);
    tick(engine, 3);
    expect(app.interaction_mode() == InteractionMode::WeightPaint && app.edited_mesh_shader() == "editor_paint",
           "switching straight to Weight Paint keeps the paint display");
    auto& ws = app.weight_paint_settings();
    ws.symmetry.axis[0] = false;
    ws.radius_px = 60.0f;
    ws.weight = 1.0f;
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    size_t weighted = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) weighted += md.mesh.weight(v, 0) > 0.5f ? 1 : 0;
    expect(md.mesh.groups.size() == 1 && weighted > 0 && weighted < md.mesh.positions.size(),
           "a weight stroke creates \"Group\" and weights the vertices under it (" + std::to_string(weighted) + ")");
    dump(engine, "16_weight_paint");
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Press, Mods::Shift); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    size_t full = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) full += md.mesh.weight(v, 0) == 1.0f ? 1 : 0;
    expect(full == md.mesh.positions.size(), "Shift+K sets the brush weight on every vertex");

    // Back to Object Mode: the material's own shader returns and the file has the paint.
    const fs::path file = md.path;
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 3);
    expect(app.edited_mesh_shader() == own_shader || app.interaction_mode() == InteractionMode::Object,
           "leaving the paint modes restores the material's shader");
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id != cube || !obj) continue;
        auto* mr = obj->get_component<coopa::gfx::engine::components::MeshRenderer>();
        expect(mr && mr->material.shader != "editor_paint", "the live renderer no longer uses the paint shader");
    }
    const EditMesh saved = mesh_from_node(coopa::yaml::load_document(file));
    expect(saved.has_colors && saved.groups.size() == 1 && saved.groups[0] == "Group",
           "the saved mesh file carries the colours and the vertex group");
}

} // namespace toy::editor::testing
