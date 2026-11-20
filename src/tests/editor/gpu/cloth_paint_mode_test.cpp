/**
 * @file cloth_paint_mode_test.cpp
 * @brief Cloth Paint through real input: the mode swaps in the paint display, a stroke paints
 *        the active channel as one undo step, Shift+K fills it, and leaving the mode saves the
 *        `cloth:` key into the mesh file.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include "editor/mesh/cloth_paint.h"
#include "editor/support/editor_session.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("cloth_paint_mode");

namespace toy::editor::testing {

/** @brief The whole workflow: enter Cloth Paint, stroke Max Distance, fill Anim Drive, undo
 *         counts, and the saved mesh file carries the `cloth:` key. */
COOPA_TEST(cloth_paint_strokes_fill_and_save_into_the_mesh) {
    using coopa::input::Key;
    using coopa::input::KeyAction;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    const std::string own_shader = app.edited_mesh_shader();
    app.set_interaction_mode(InteractionMode::ClothPaint);
    tick(engine, 4);
    expect(app.interaction_mode() == InteractionMode::ClothPaint, "the mode enters Cloth Paint");
    expect(app.edited_mesh_shader() == "editor_paint",
           "Cloth Paint draws the mesh with the paint display shader");
    auto& md = app.mesh_document();
    app.subdivide_smooth(2);   // vertices for the brush to land on
    tick(engine, 3);
    expect(!md.mesh.has_cloth(), "the default cube starts without cloth paint");

    auto& cs = app.cloth_paint_settings();
    cs.symmetry.axis[0] = false;
    cs.radius_px = 60.0f;
    app.set_active_cloth_channel(static_cast<int>(ClothChannel::MaxDistance));
    app.cloth_paint_value(static_cast<int>(ClothChannel::MaxDistance)) = 0.25f;

    const glm::vec2 corner = visible_corner(app, cube);
    auto start = screen_of(engine, app, cube, glm::vec3(corner.x, corner.y * 0.3f, 0.0f), in.scale);
    auto stop = screen_of(engine, app, cube, glm::vec3(corner.x * 0.3f, corner.y, 0.0f), in.scale);
    expect(start && stop, "the stroke lies on screen");
    if (!start || !stop) return;

    const size_t undo0 = md.undo.undo_count();
    in.move(*start);
    in.drag(*stop, MouseButton::Left, 10);
    tick(engine, 2);
    size_t painted = 0, untouched = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        const float x = cloth_channel_value(md.mesh, ClothChannel::MaxDistance, v);
        painted += x > 0.2f ? 1 : 0;
        untouched += x == 0.0f ? 1 : 0;
    }
    expect(md.mesh.has_cloth() && painted > 0,
           "a stroke paints the channel (" + std::to_string(painted) + " vertices)");
    expect(untouched > 0, "...only under the brush");
    expect(md.undo.undo_count() == undo0 + 1, "the whole stroke is one undo step");
    dump(engine, "17_cloth_paint");

    // Shift+K fills the active channel with the brush value: switch to Anim Drive and fill.
    app.set_active_cloth_channel(static_cast<int>(ClothChannel::AnimDrive));
    app.cloth_paint_value(static_cast<int>(ClothChannel::AnimDrive)) = 0.7f;
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Press, Mods::Shift); });
    tick(engine, 1);
    engine.queue_input([](coopa::input::Input& i) { i.push_key(Key::K, 0, KeyAction::Release, Mods::None); });
    tick(engine, 2);
    size_t driven = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        driven += std::abs(cloth_channel_value(md.mesh, ClothChannel::AnimDrive, v) - 0.7f) < 1e-4f ? 1 : 0;
    }
    expect(driven == md.mesh.positions.size(), "Shift+K sets the brush value on every vertex");

    // Back to Object Mode: the shader returns and the file carries the cloth key.
    const fs::path file = md.path;
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 3);
    expect(app.edited_mesh_shader() == own_shader || app.interaction_mode() == InteractionMode::Object,
           "leaving Cloth Paint restores the material's shader");
    const EditMesh saved = mesh_from_node(coopa::yaml::load_document(file));
    expect(saved.has_cloth() && saved.cloth_anim_drive.size() == saved.positions.size(),
           "the saved mesh file carries the cloth channels");
    size_t saved_painted = 0;
    for (uint32_t v = 0; v < saved.positions.size(); ++v) {
        saved_painted += cloth_channel_value(saved, ClothChannel::MaxDistance, v) > 0.2f ? 1 : 0;
    }
    expect(saved_painted > 0, "the painted max_distance values reached the file");
}

} // namespace toy::editor::testing
