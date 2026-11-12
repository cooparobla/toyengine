/**
 * @file modelling_test.cpp
 * @brief Modelling through real input and the Blender keymap: object G / R / S with typed values and
 * RMB cancel, Ctrl+Z, orbit and zoom, Edit Mode extrude, rotate with axis locks, X symmetry and
 * Mirror, Ctrl+R loop cut and slide, Alt+click loops, Ctrl+Alt rings, Ctrl+T / Alt+J, and
 * proportional editing.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"
#include "editor/support/mesh_checks.h"

COOPA_TEST_SUITE("modelling");

namespace toy::editor::testing {

/** @brief The Blender keymap through real input on the starter cube: menus open where drawn, a
 *         click selects, G X 2 / R Z 90 with typed values, RMB cancels, Ctrl+Z undoes, MMB orbits,
 *         the wheel zooms (fractional deltas too), Shift+A, and Tab / E / Ctrl+Z in Edit Mode. */
COOPA_TEST(blender_keymap_through_real_input) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    // Menubar: clicking "File" where it is drawn opens it (catches any hover offset).
    in.click({44, 14});
    expect(app.ui().any_popup_open(), "clicking the File menu header opens it");
    in.key(Key::Escape);
    in.click({4, 300});   // click elsewhere closes it
    tick(engine, 2);

    const imm::Box vp = app.viewport_box();
    expect(vp.w > 100 && vp.h > 100, "the viewport has a usable size");
    const glm::vec2 centre = vp.center();

    // Click-select the cube at its projected centre.
    const ObjectId cube = object_named(app, "cube");
    glm::vec2 px;
    expect(engine.world_to_window(glm::vec3(0, 0, 0.5f), px), "the cube projects into the viewport");
    in.click(px / in.scale);
    expect(app.document().primary() == cube, "a real left click on the cube selects it");

    // G, X, type 2, Enter: moved exactly 2 along X.
    in.move(centre);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Num2);
    in.key(Key::Enter);
    glm::vec3 p, r, s;
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(p.x - 2.0f) < 1e-4f && std::abs(p.y) < 1e-4f && std::abs(p.z - 0.5f) < 1e-4f,
           "G X 2 Enter moves the cube exactly 2 on X (got " + std::to_string(p.x) + ", " + std::to_string(p.y) + ")");
    // G with mouse motion then RMB cancels.
    in.key(Key::G);
    in.move(centre + glm::vec2(80, 30), 2);
    in.click(centre + glm::vec2(80, 30), MouseButton::Right);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(p.x - 2.0f) < 1e-4f, "RMB cancels a grab and restores the position");
    // R Z 90 Enter.
    in.move(centre);
    in.key(Key::R);
    in.key(Key::Z);
    in.key(Key::Num9);
    in.key(Key::Num0);
    in.key(Key::Enter);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(std::abs(r.z) - 90.0f) < 0.01f, "R Z 90 rotates 90 degrees about Z (got " + std::to_string(r.z) + ")");
    // Ctrl+Z undoes the rotation.
    in.key(Key::Z, Mods::Control);
    tick(engine, 2);
    app.document().get_transform(cube, p, r, s);
    expect(std::abs(r.z) < 0.01f, "Ctrl+Z undoes it");

    // MMB drag orbits the view.
    const float yaw0 = app.camera().yaw_deg;
    in.move(centre);
    in.drag(centre + glm::vec2(120, 0), MouseButton::Middle);
    expect(std::abs(app.camera().yaw_deg - yaw0) > 10.0f, "middle-drag orbits the camera");

    // The wheel zooms -- whole notches and macOS-style fractional (smooth) deltas alike.
    for (float notch : {1.0f, 0.35f, -0.35f}) {
        in.move(centre);
        const float d0 = app.camera().distance;
        const float yaw_before = app.camera().yaw_deg;
        engine.queue_input([notch](coopa::input::Input& i) { i.push_scroll(0.0, notch); });
        tick(engine, 1);
        const float d1 = app.camera().distance;
        expect(notch > 0 ? d1 < d0 : d1 > d0, "wheel " + std::to_string(notch) + " zooms " + (notch > 0 ? "in" : "out") +
               " (" + std::to_string(d0) + " -> " + std::to_string(d1) + ")");
        expect(app.camera().yaw_deg == yaw_before, "and does not orbit");
    }

    // Shift+A opens the add menu at the mouse.
    in.move(centre);
    in.key(Key::A, Mods::Shift);
    expect(app.ui().any_popup_open(), "Shift+A opens the add menu");
    in.key(Key::Escape);
    in.click({4, 300});

    // Tab into edit mode, face mode, select all, E extrude along the normal by 1.
    in.move(centre);
    app.document().select(cube);
    in.key(Key::Tab);
    expect(app.mesh_document().open() && app.mesh_document().mesh.faces.size() == 6, "Tab enters edit mode on the cube's mesh");
    in.key(Key::Num3);
    app.mesh_document().selection.faces = {1};   // the +Z face
    in.key(Key::E);
    in.key(Key::Num1);
    in.key(Key::Enter);
    glm::vec3 lo, hi;
    app.mesh_document().mesh.bounds(lo, hi);
    expect(app.mesh_document().mesh.faces.size() == 10 && std::abs(hi.z - 1.5f) < 1e-3f,
           "E 1 Enter extrudes the top face 1 unit along its normal (top z " + std::to_string(hi.z) + ")");
    in.key(Key::Z, Mods::Control);
    app.mesh_document().mesh.bounds(lo, hi);
    expect(std::abs(hi.z - 0.5f) < 1e-3f, "Ctrl+Z in edit mode undoes the mesh edit");
    expect(app.edit_mode_active(), "still in edit mode before the second Tab");
    in.key(Key::Tab);
    expect(!app.edit_mode_active(), "Tab leaves edit mode");
    dump(engine, "06_after_input");
}

/** @brief Edit Mode R: rotate the selection with axis locks and typed angles like G (on a scene
 *         object and on a mesh asset), RMB cancels, X symmetry widens both sides, Ctrl M mirrors. */
COOPA_TEST(edit_mode_rotate_symmetry_and_mirror) {
    using coopa::input::Key;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    const glm::vec2 c = app.viewport_box().center();
    in.move(c);
    in.key(Key::Tab);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    expect(app.edit_mode_active(), "Tab enters Edit Mode");
    auto& md = app.mesh_document();
    in.key(Key::Num1);
    in.key(Key::A);
    const EditMesh before = md.mesh;

    // R Z 90 Enter: a quarter turn about Z through the selection's centre.
    in.move(c + glm::vec2(60, 0));
    in.key(Key::R);
    expect(app.modal_active(), "R starts Rotate in Edit Mode");
    in.key(Key::Z);
    in.key(Key::Num9);
    in.key(Key::Num0);
    in.key(Key::Enter);
    expect(!app.modal_active(), "Enter confirms");
    glm::vec3 centre(0.0f);
    for (const auto& p : before.positions) centre += p;
    centre /= static_cast<float>(before.positions.size());
    bool ok = md.mesh.positions.size() == before.positions.size();
    for (size_t i = 0; ok && i < before.positions.size(); ++i) {
        const glm::vec3 d = before.positions[i] - centre;
        const glm::vec3 want = centre + glm::vec3(-d.y, d.x, d.z);
        ok = glm::length(md.mesh.positions[i] - want) < 1e-3f;
    }
    expect(ok, "R Z 90 rotates every selected vertex 90 degrees about Z");

    // Mouse-driven rotate, then RMB cancels back to the rotated state.
    const EditMesh rotated = md.mesh;
    in.key(Key::R);
    in.move(c + glm::vec2(10, 70), 3);
    bool moved = false;
    for (size_t i = 0; i < rotated.positions.size(); ++i) moved |= glm::length(md.mesh.positions[i] - rotated.positions[i]) > 1e-3f;
    expect(moved, "moving the mouse during R rotates live");
    in.click(c + glm::vec2(10, 70), coopa::input::MouseButton::Right);
    bool restored = true;
    for (size_t i = 0; i < rotated.positions.size(); ++i) restored &= glm::length(md.mesh.positions[i] - rotated.positions[i]) < 1e-5f;
    expect(restored, "RMB cancels the rotate");

    // The same from a mesh asset (Meshes tab, Edit mode).
    in.key(Key::Tab);
    app.save_mesh();  // the rotated cube mesh is dirty; don't stop at the save prompt
    app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);
    expect(app.edit_mode_active(), "mesh asset: Edit mode");
    in.move(c);
    in.key(Key::A);
    const EditMesh asset_before = app.mesh_document().mesh;
    in.move(c + glm::vec2(60, 0));
    in.key(Key::R);
    expect(app.modal_active(), "mesh asset: R starts Rotate");
    in.key(Key::X);
    in.key(Key::Num4);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool changed = false;
    for (size_t i = 0; i < asset_before.positions.size(); ++i)
        changed |= glm::length(app.mesh_document().mesh.positions[i] - asset_before.positions[i]) > 1e-3f;
    expect(changed, "mesh asset: R X 45 rotates the selection");

    // X mirror on: G X on the +X vertices moves the -X ones the opposite way.
    in.key(Key::Z, coopa::input::Mods::Control);   // undo the rotate
    auto& amd = app.mesh_document();
    const EditMesh sym_before = amd.mesh;
    amd.selection.clear();
    amd.selection.mode = SelectMode::Vertex;
    for (uint32_t v = 0; v < amd.mesh.positions.size(); ++v) if (amd.mesh.positions[v].x > 0) amd.selection.verts.insert(v);
    app.edit_symmetry().axis[0] = true;
    tick(engine, 2);
    dump(engine, "mesh_symmetry_header");
    in.move(c);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Period);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool sym_ok = true;
    for (uint32_t v = 0; v < sym_before.positions.size(); ++v) {
        const float dx = amd.mesh.positions[v].x - sym_before.positions[v].x;
        sym_ok &= std::abs(dx - (sym_before.positions[v].x > 0 ? 0.5f : -0.5f)) < 1e-4f;
    }
    expect(sym_ok, "with X mirror on, G X .5 widens both sides symmetrically");
    app.edit_symmetry().axis[0] = false;
    app.set_interaction_mode(InteractionMode::Sculpt);
    tick(engine, 3);
    dump(engine, "mesh_symmetry_sculpt_header");
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);

    // Ctrl M opens Mirror; X Local flips the selection.
    in.key(Key::M, coopa::input::Mods::Control);
    expect(app.ui().any_popup_open(), "Ctrl M opens the Mirror menu");
    in.key(Key::Escape);
    // Mirror the +X half's -Y vertices along X: they cross the selection centre (x of the +X face).
    amd.selection.verts.clear();
    for (uint32_t v = 0; v < amd.mesh.positions.size(); ++v) if (amd.mesh.positions[v].y < 0) amd.selection.verts.insert(v);
    glm::vec3 sc(0.0f);
    for (uint32_t v : amd.selection.verts) sc += amd.mesh.positions[v];
    sc /= static_cast<float>(amd.selection.verts.size());
    const EditMesh pre_mirror = amd.mesh;
    app.mirror_mesh_selection(0, false);
    bool flipped = !amd.selection.verts.empty();
    for (uint32_t v : amd.selection.verts)
        flipped &= std::abs(amd.mesh.positions[v].x - (2.0f * sc.x - pre_mirror.positions[v].x)) < 1e-4f &&
                   std::abs(amd.mesh.positions[v].y - pre_mirror.positions[v].y) < 1e-6f;
    expect(flipped, "Mirror X Local reflects the selection across its centre plane");
}

/** @brief Ctrl+R loop cut and slide (wheel, Esc, RMB), one undo step per cut, Alt+click loops,
 *         G G edge slide, Ctrl+Alt+click rings, Ctrl+T / Alt+J -- with real input. */
COOPA_TEST(loop_cut_slide_rings_and_triangulate) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    in.move(app.viewport_box().center());
    in.key(Key::Tab);
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    expect(app.edit_mode_active(), "Tab enters Edit Mode");
    in.key(Key::Num2);
    auto& md = app.mesh_document();
    expect(md.selection.mode == SelectMode::Edge, "2: edge select mode");

    const glm::vec2 corner = visible_corner(app, cube);
    const auto edge_px = screen_of(engine, app, cube, glm::vec3(corner, 0.1f), in.scale);
    expect(edge_px.has_value(), "the cube's near vertical edge is on screen");
    if (!edge_px) return;

    // Ctrl+R: the wheel changes the cut count, Esc cancels.
    in.move(*edge_px);
    in.key(Key::R, Mods::Control);
    expect(app.loop_cut_active(), "Ctrl+R starts Loop Cut and Slide");
    engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0, 1); });
    tick(engine, 2);
    expect(app.loop_cut_cuts() == 2, "the wheel adds a cut (got " + std::to_string(app.loop_cut_cuts()) + ")");
    in.key(Key::Escape);
    expect(!app.loop_cut_active() && md.mesh.faces.size() == 6, "Esc cancels without cutting");

    // Ctrl+R, LMB (cut, then slide), RMB (keep the cut centred).
    in.move(*edge_px);
    in.key(Key::R, Mods::Control);
    in.move(*edge_px + glm::vec2(1, 0));
    in.click(*edge_px);
    in.click(*edge_px, MouseButton::Right);
    expect(md.mesh.faces.size() == 10 && closed_and_consistent(md.mesh), "one cut around the cube: 10 faces, closed (got " +
                                                                          std::to_string(md.mesh.faces.size()) + ")");
    bool centred = true;
    for (uint32_t v = 8; v < md.mesh.positions.size(); ++v) centred &= std::abs(md.mesh.positions[v].z) < 1e-4f;
    expect(centred, "RMB after the cut leaves the loop centred");
    in.key(Key::Z, Mods::Control);
    expect(md.mesh.faces.size() == 6, "one undo step removes the cut and its slide");
    in.key(Key::Z, Mods::Control | Mods::Shift);
    expect(md.mesh.faces.size() == 10, "redo brings it back");

    // Alt+click a loop edge: the whole new loop, without orbiting.
    const float yaw = app.camera().yaw_deg;
    const auto loop_px = screen_of(engine, app, cube, glm::vec3(corner.x, 0.0f, 0.0f), in.scale);
    if (loop_px) in.click(*loop_px, MouseButton::Left, Mods::Alt);
    in.key(Key::Num2);   // (a key event clears the held Alt)
    expect(md.selection.edges.size() == 4 && std::abs(app.camera().yaw_deg - yaw) < 1e-4f,
           "Alt+click selects the 4-edge loop and the view stays put (got " + std::to_string(md.selection.edges.size()) + ")");

    // G G 0.5 Enter: Edge Slide halfway.
    in.move(app.viewport_box().center());
    in.key(Key::G);
    in.key(Key::G);
    in.key(Key::Period);
    in.key(Key::Num5);
    in.key(Key::Enter);
    bool slid = true;
    for (uint32_t v = 8; v < 12; ++v) slid &= std::abs(std::abs(md.mesh.positions[v].z) - 0.25f) < 1e-4f;
    expect(slid, "G G .5 slides the loop halfway along its rails");

    // Ctrl+Alt+click a vertical edge: its ring.
    const auto ring_px = screen_of(engine, app, cube, glm::vec3(corner, -0.3f), in.scale);
    if (ring_px) in.click(*ring_px, MouseButton::Left, Mods::Control | Mods::Alt);
    in.key(Key::Num2);
    expect(md.selection.edges.size() == 4, "Ctrl+Alt+click selects the edge ring (got " + std::to_string(md.selection.edges.size()) + ")");

    // Ctrl+T: triangulate everything.
    in.key(Key::Num3);
    in.key(Key::A);
    in.key(Key::T, Mods::Control);
    expect(md.mesh.faces.size() == 20 && closed_and_consistent(md.mesh), "Ctrl+T: 10 quads become 20 triangles");
    in.key(Key::J, Mods::Alt);
    expect(md.mesh.faces.size() == 10, "Alt+J joins them back into quads");
    dump(engine, "12_quad_modelling");
    // Subdivide (W menu op) shows the Adjust Last Operation panel.
    in.key(Key::A);
    in.move(app.viewport_box().center());
    in.key(Key::W);
    tick(engine, 2);
    expect(app.ui().any_popup_open(), "W opens the edit-mode context menu");
    in.key(Key::Escape);
    in.key(Key::Tab);
    tick(engine, 2);
    const ObjectId grid = app.create_primitive("Grid");
    tick(engine, 3);
    expect(grid != 0, "Add > Grid creates a grid object");
    dump(engine, "15_adjust_last_operation");
}

/**
 * @brief Proportional editing through real input: O turns it on, G Z 1 Enter on one vertex of
 *        a grid lifts its neighbours by the falloff, the wheel resizes the circle mid-grab.
 */
COOPA_TEST(proportional_editing_through_real_input) {
    using coopa::input::Key;
    EditorSession session({.fixed_dt = "0", .prepare = [](Project& project) {
        coopa::yaml::save_document(project.assets() / "meshes" / "grid.yaml", mesh_to_node(make_grid(10, 10, 10.0f)));
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    app.open_asset(AssetType::Mesh, "meshes/grid.yaml");
    tick(engine, toy::core::Engine::kFillDebounceFrames + 2);
    app.set_interaction_mode(InteractionMode::Edit);
    tick(engine, 2);
    expect(app.edit_mode_active(), "the grid is in Edit Mode");
    auto& md = app.mesh_document();
    uint32_t centre = 0, near = 0, far = 0;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        const glm::vec3 p = md.mesh.positions[v];
        if (glm::length(p) < 1e-4f) centre = v;
        if (glm::length(p - glm::vec3(1, 0, 0)) < 1e-4f) near = v;
        if (glm::length(p - glm::vec3(5, 0, 0)) < 1e-4f) far = v;
    }
    md.selection.clear();
    md.selection.mode = SelectMode::Vertex;
    md.selection.verts.insert(centre);
    const glm::vec2 c = app.viewport_box().center();
    in.move(c);
    expect(!app.proportional().enabled, "proportional editing starts off");
    in.key(Key::O);
    expect(app.proportional().enabled, "O turns it on");
    app.proportional().falloff = Falloff::Linear;
    app.proportional().projected = false;
    app.proportional().radius = 3.0f;
    const EditMesh before = md.mesh;
    in.key(Key::G);
    in.key(Key::Z);
    in.key(Key::Num1);
    in.key(Key::Enter);
    expect(std::abs(md.mesh.positions[centre].z - 1.0f) < 1e-4f, "the selected vertex moves the full 1");
    expect(std::abs(md.mesh.positions[near].z - 2.0f / 3.0f) < 1e-3f,
           "a vertex 1 m away moves 2/3 (linear falloff, radius 3) -- got " + std::to_string(md.mesh.positions[near].z));
    expect(std::abs(md.mesh.positions[far].z) < 1e-6f, "a vertex outside the radius stays put");
    in.key(Key::Z, coopa::input::Mods::Control);
    tick(engine, 2);
    expect(std::abs(md.mesh.positions[near].z) < 1e-6f && std::abs(md.mesh.positions[centre].z) < 1e-6f,
           "one undo puts the neighbours back too");

    // The wheel resizes the circle while grabbing (EDITOR_DUMP_DIR shows the circle).
    app.proportional().projected = true;
    app.proportional().falloff = Falloff::Smooth;
    in.move(c);
    in.key(Key::G);
    in.move(c + glm::vec2(0, -60), 3);
    const float r0 = app.proportional().radius;
    engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0.0, 3.0); });
    tick(engine, 2);
    expect(app.proportional().radius > r0 * 1.2f, "scrolling up during G grows the radius");
    dump(engine, "proportional_grab");
    in.key(Key::Escape);
    bool restored = true;
    for (size_t i = 0; i < before.positions.size(); ++i) restored &= glm::length(md.mesh.positions[i] - before.positions[i]) < 1e-5f;
    expect(restored, "Esc puts every vertex back");
    in.key(Key::O, coopa::input::Mods::Shift);
    expect(app.proportional().falloff == Falloff::Sphere, "Shift O cycles the falloff");
    in.key(Key::O);
    expect(!app.proportional().enabled, "O again turns it off");
}

} // namespace toy::editor::testing
