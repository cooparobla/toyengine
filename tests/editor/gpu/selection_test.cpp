/**
 * @file selection_test.cpp
 * @brief Picking and what selection shows: a click picks the nearest visible surface, hidden objects and
 * occluded markers never steal it, tiled water picks as one object, X-Ray lets hidden elements be
 * clicked and boxed, Edit Mode isolates the mesh, and the Colliders overlay follows the selection.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("selection");

namespace toy::editor::testing {

/** @brief A click picks the nearest visible surface: not a hidden occluder, nor a marker behind it. */
COOPA_TEST(a_click_picks_the_nearest_visible_surface) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto* cam = coopa::gfx::engine::components::CameraComponent::main();
    expect(cam != nullptr, "a main camera (the editor camera) exists");
    if (!cam) return;
    auto name_of = [&](ObjectId id) {
        return id && app.document().find(id) ? get_string(*app.document().find(id), "name") : std::string("nothing");
    };
    auto place = [&](ObjectId id, glm::vec3 p) {
        app.document().set_transform(id, p, {0, 0, 0}, glm::vec3(1), "Move");
        app.sync().apply(engine, app.document(), {ChangeScope::Transform, id});
    };
    // Two spheres on one camera ray, the near one between the camera and the far one.
    const glm::vec3 eye = cam->get_world_position();
    const glm::vec3 far_pos(0, 0, 3);
    const glm::vec3 to_eye = glm::normalize(eye - far_pos);
    const ObjectId far_s = app.create_primitive("Sphere");
    const ObjectId near_s = app.create_primitive("Sphere");
    place(far_s, far_pos);
    place(near_s, far_pos + to_eye * 4.0f);
    tick(engine, 2);
    glm::vec2 px;
    expect(engine.world_to_window(far_pos, px), "the far sphere is on screen");
    const float s = std::max(1.0f, engine.display_scale());
    ObjectId got = app.pick_object(px / s);
    expect(got == near_s, "the nearer of two spheres on the ray is picked (got " + name_of(got) + ")");

    app.hide_objects({near_s});
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == far_s, "a hidden sphere is not picked; the one behind it is (got " + name_of(got) + ")");

    // A mesh-less object's marker exactly under the cursor but BEHIND the surface loses to it.
    ObjectId sun = 0;
    for (ObjectId id : app.document().all_ids()) if (name_of(id) == "sun") sun = id;
    expect(sun != 0, "the default scene has a sun");
    place(sun, far_pos - to_eye * 3.0f);
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == far_s, "a marker hidden behind a surface does not steal the click (got " + name_of(got) + ")");
    // In front of it, the marker still wins a dead-on click.
    place(sun, far_pos + to_eye * 2.0f);
    tick(engine, 2);
    got = app.pick_object(px / s);
    expect(got == sun, "a marker in front of the surface wins a dead-on click (got " + name_of(got) + ")");
}

/** @brief A water body larger than one render tile draws as runtime tile children: they render,
 *         clicking the water still picks the water object, and they never reach the saved scene. */
COOPA_TEST(tiled_water_picks_as_one_object_and_never_saves_its_tiles) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    const fs::path root = session.project.root();

    const ObjectId lake = app.create_primitive("Plane");
    auto& doc = app.document();
    const int mr_ci = doc.find_component(lake, "MeshRenderer");
    Node mr = doc.find(lake)->at("components").as_seq()[static_cast<size_t>(mr_ci)];
    mr["mesh_path"] = Node(std::string(""));
    doc.set_component(lake, mr_ci, mr, "No mesh");
    Node water = Node::mapping();
    water["type"] = Node(std::string("WaterBody"));
    water["mode"] = Node(std::string("planar"));
    Node size = Node::mapping();
    size["x"] = make_float(120.0);
    size["y"] = make_float(120.0);
    water["size"] = size;
    water["resolution"] = Node(static_cast<int64_t>(24));
    doc.add_component(lake, water);
    doc.set_transform(lake, {0.0f, 80.0f, 0.5f}, {0, 0, 0}, glm::vec3(1), "Move");
    app.sync().rebuild(engine, doc);
    tick(engine, 3);
    // A grid body has no surface the editor can pick (only its origin marker): one round trip
    // through Edit Mode writes the grid to a mesh file, as an author editing the shape would.
    // That also makes it a mesh-sourced body, tiled by triangle with simplified LODs.
    doc.select(lake);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode converts the grid to a mesh");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 4);

    toy::water::WaterBody* live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == lake && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    expect(live && live->tiles.size() > 1u, "a 120 m lake is drawn as several tiles (" +
                                                std::to_string(live ? live->tiles.size() : 0u) + ")");
    expect(live && toy::water::WaterSystem::is_published(*live), "...every one of them published");
    bool placed = live != nullptr;
    if (live) {
        for (const auto& t : live->tiles) {
            const glm::vec3 p(t.object->get_transform()->transform().get_world_matrix()[3]);
            placed = placed && glm::distance(p, glm::vec3(0.0f, 80.0f, 0.5f)) < 1e-4f;
        }
    }
    expect(placed, "...placed with the lake (tiles follow the owner's transform)");

    // Clicking the water picks the lake itself (picking is per document object).
    app.camera().focus = glm::vec3(10.0f, 80.0f, 0.5f);
    app.camera().distance = 60.0f;
    app.camera().pitch_deg = 50.0f;
    app.camera().apply();
    tick(engine, 2);
    glm::vec2 px;
    const bool on_screen = engine.world_to_window(glm::vec3(10.0f, 80.0f, 0.5f), px);
    expect(on_screen, "the lake is on screen");
    if (on_screen) {
        const float s = std::max(1.0f, engine.display_scale());
        expect(app.pick_object(px / s) == lake, "clicking the tiled water's surface picks the water object");
    }

    // The tiles are runtime only: the saved scene has the lake and no tiles.
    const fs::path out = root / "tiles_saved.yaml";
    expect(app.save_scene_as(out), "the scene saves");
    std::ifstream in(out);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(text.find("WaterBody") != std::string::npos && text.find("water_tile") == std::string::npos,
           "...with the water body and none of its render tiles");
}

/** @brief Blender's X-Ray in Edit Mode: Alt+Z makes the surface translucent, and what is behind it
 *         is drawn (dimmed) and can be clicked. */
COOPA_TEST(xray_lets_hidden_elements_be_clicked_and_boxed) {
    using coopa::input::Key;
    using coopa::input::Mods;
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
    in.key(Key::Num1);   // vertex select
    tick(engine, 2);

    auto& md = app.mesh_document();
    // The vertex farthest from the camera: behind the cube's surface.
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 w = world_of(app, cube);
    uint32_t back = 0;
    float bd = -1.0f;
    for (uint32_t v = 0; v < md.mesh.positions.size(); ++v) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(md.mesh.positions[v], 1.0f)), eye);
        if (d > bd) { bd = d; back = v; }
    }
    const auto back_px = screen_of(engine, app, cube, md.mesh.positions[back], in.scale);
    expect(back_px.has_value(), "the hidden vertex projects on screen");
    if (!back_px) return;
    auto pixel = [&](glm::vec2 p) {
        const auto img = engine.capture_image(false);
        const int x = std::clamp(static_cast<int>(p.x * in.scale), 0, static_cast<int>(img.width) - 1);
        const int y = std::clamp(static_cast<int>(p.y * in.scale), 0, static_cast<int>(img.height) - 1);
        const uint8_t* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * img.channels];
        return glm::vec3(q[0], q[1], q[2]);
    };
    const glm::vec2 body = app.viewport_box().center() + glm::vec2(9.0f, 7.0f);   // on the cube, off the overlay
    const glm::vec3 opaque = pixel(body);

    md.selection.verts.clear();
    in.click(*back_px);
    tick(engine, 2);
    expect(!md.selection.verts.count(back), "without X-Ray the hidden vertex can't be clicked");

    in.move(app.viewport_box().center());
    in.key(Key::Z, Mods::Alt);
    tick(engine, 3);
    expect(engine.render_config().editor_xray_alpha < 0.99f, "Alt+Z turns X-Ray on: the surface is translucent");
    const glm::vec3 xray = pixel(body);
    expect(glm::length(xray - opaque) > 6.0f, "the X-Ray surface lets the backdrop through");
    dump(engine, "23_xray_edit");

    md.selection.verts.clear();
    in.click(*back_px);
    tick(engine, 2);
    expect(md.selection.verts.count(back) == 1, "with X-Ray the vertex behind the surface is clicked");
    // Box select: a drag over the whole cube takes the hidden vertex only with X-Ray.
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const auto& p : md.mesh.positions)
        if (auto q = screen_of(engine, app, cube, p, in.scale)) { lo = glm::min(lo, *q); hi = glm::max(hi, *q); }
    auto box_all = [&] {
        md.selection.verts.clear();
        in.move(lo - glm::vec2(15.0f));
        in.drag(hi + glm::vec2(15.0f), coopa::input::MouseButton::Left);
        tick(engine, 2);
    };
    box_all();
    expect(md.selection.verts.size() == md.mesh.positions.size(), "with X-Ray a box takes every vertex, hidden ones too");

    // Faces: X-Ray picks the face whose centre is nearest the click on screen, even one behind
    // the face the ray would hit first.
    in.key(Key::Num3);
    tick(engine, 2);
    uint32_t back_face = 0;
    float fd = -1.0f;
    for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) {
        const float d = glm::distance(glm::vec3(w * glm::vec4(md.mesh.face_center(f), 1.0f)), eye);
        if (d > fd) { fd = d; back_face = f; }
    }
    const auto face_px = screen_of(engine, app, cube, md.mesh.face_center(back_face), in.scale);
    expect(face_px.has_value(), "the hidden face's centre projects on screen");
    if (face_px) {
        md.selection.faces.clear();
        in.click(*face_px);
        tick(engine, 2);
        expect(md.selection.faces.size() == 1 && md.selection.faces.count(back_face),
               "with X-Ray a click on a hidden face's dot selects that face");
    }

    in.move(app.viewport_box().center());
    in.key(Key::Z, Mods::Alt);
    tick(engine, 3);
    expect(engine.render_config().editor_xray_alpha >= 1.0f, "Alt+Z again: opaque");
    if (face_px) {
        md.selection.faces.clear();
        in.click(*face_px);
        tick(engine, 2);
        expect(md.selection.faces.size() == 1 && !md.selection.faces.count(back_face),
               "without X-Ray the same click takes the face in front");
    }
    in.key(Key::Num1);
    tick(engine, 2);
    box_all();
    expect(!md.selection.verts.empty() && !md.selection.verts.count(back), "without X-Ray a box takes only the vertices in view");
}

/** @brief Edit Mode isolates the mesh (lights stay), Tab restores, and the toggle persists. */
COOPA_TEST(edit_mode_isolates_the_mesh_and_tab_restores_the_view) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    const ObjectId cube = object_named(app, "cube"), ground = object_named(app, "ground"), sun = object_named(app, "sun");
    expect(cube && ground && sun, "the new project has Cube, Ground and Sun");
    app.hide_objects({object_named(app, "camera")});
    const float yaw0 = app.camera().yaw_deg, dist0 = app.camera().distance;
    app.document().select(cube);
    in.move(app.viewport_box().center());
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.edit_mode_active() && app.is_isolated(ground) && !app.sync().live(ground)->active(), "Edit Mode hides the other mesh");
    expect(!app.is_isolated(sun) && app.sync().live(sun)->active(), "lights stay on");
    expect(std::abs(app.camera().distance - dist0) > 1e-3f, "the view frames the edited mesh");
    dump(engine, "13_isolated_edit_mode");
    in.key(Key::Tab);
    tick(engine, 2);
    expect(!app.edit_mode_active() && app.sync().live(ground)->active() && !app.is_isolated(ground), "Tab brings everything back");
    expect(std::abs(app.camera().yaw_deg - yaw0) < 1e-3f && std::abs(app.camera().distance - dist0) < 1e-3f, "and restores the view");
    expect(!app.sync().live(object_named(app, "camera"))->active(), "an object the user hid stays hidden");

    app.set_isolate_in_edit(false);
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.edit_mode_active() && app.sync().live(ground)->active(), "with the toggle off, Edit Mode leaves the scene visible");
    in.key(Key::Tab);
    const Node prefs = Project::load_prefs();
    expect(prefs.contains("isolate_edit_mode") && !prefs.at("isolate_edit_mode").get_value<bool>(), "the toggle is saved");
}

/**
 * @brief The viewport header's Colliders toggle: off by default; on, it draws every collider
 *        (edit and play); "Selected + Children" draws just the selection's and its children's.
 */
COOPA_TEST(the_collider_overlay_follows_toggle_selection_and_play) {
    EditorSession session({.fixed_dt = "0", .prepare = [](Project& project) {
        write_text(project.assets() / "scenes" / "colliders" / "scene.yaml",
            "format: blender\n"
            "scene:\n"
            "  scene_name: colliders\n"
            "  root_objects:\n"
            "    - name: crate\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: 0.0, y: 0.0, z: 1.0}}\n"
            "        - {type: BoxCollider, size: {x: 1.0, y: 2.0, z: 1.0}}\n"
            "      children:\n"
            "        - name: ball\n"
            "          active: true\n"
            "          components:\n"
            "            - {type: Transform, position: {x: 2.0, y: 0.0, z: 0.0}}\n"
            "            - {type: SphereCollider, radius: 0.5}\n"
            "          children: []\n"
            "    - name: trigger_pill\n"
            "      active: true\n"
            "      components:\n"
            "        - {type: Transform, position: {x: -3.0, y: 0.0, z: 1.0}}\n"
            "        - {type: CapsuleCollider, radius: 0.3, height: 1.5, is_trigger: true}\n"
            "      children: []\n");
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    const fs::path scene_file = session.project.assets() / "scenes" / "colliders" / "scene.yaml";
    expect(app.open_scene(scene_file), "colliders: the scene opens");
    tick(engine, 3);
    expect(!app.show_colliders() && app.collider_lines_drawn() == 0, "colliders: off by default");

    // The header toggle, clicked where it is drawn.
    const auto btn = app.test_rect("colliders");
    expect(btn.has_value(), "colliders: the toggle is in the viewport header");
    if (!btn) return;
    in.click(btn->center());
    tick(engine, 2);
    const size_t all = app.collider_lines_drawn();
    expect(app.show_colliders() && all > 0, "colliders: the toggle draws them (" + std::to_string(all) + " lines)");

    // Selected + children: the crate brings its child ball; the trigger pill alone is the rest.
    app.set_colliders_selected_only(true);
    const ObjectId crate = object_named(app, "crate"), pill = object_named(app, "trigger_pill");
    expect(crate && pill, "colliders: the objects are in the scene");
    app.document().select(crate, false);
    tick(engine, 2);
    const size_t crate_and_child = app.collider_lines_drawn();
    app.document().select(pill, false);
    tick(engine, 2);
    const size_t pill_only = app.collider_lines_drawn();
    expect(crate_and_child > 0 && pill_only > 0 && crate_and_child + pill_only == all,
           "colliders: the selection's and its children's only (" + std::to_string(crate_and_child) + " + " +
               std::to_string(pill_only) + " of " + std::to_string(all) + ")");
    app.document().clear_selection();
    tick(engine, 2);
    expect(app.collider_lines_drawn() == 0, "colliders: nothing selected, nothing drawn");

    // While playing: the play scene's colliders, the selection found in it.
    app.document().select(crate, false);
    app.play();
    tick(engine, 4);
    expect(app.playing() && app.collider_lines_drawn() == crate_and_child, "colliders: the selection's in play too");
    app.set_colliders_selected_only(false);
    tick(engine, 2);
    expect(app.collider_lines_drawn() == all, "colliders: every collider of the play scene");
    app.stop();
    tick(engine, 2);

    in.click(btn->center());
    tick(engine, 2);
    expect(!app.show_colliders() && app.collider_lines_drawn() == 0, "colliders: the toggle turns them off again");
}

} // namespace toy::editor::testing
