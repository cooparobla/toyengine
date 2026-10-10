/**
 * @file viewport_test.cpp
 * @brief The viewport: the render fills its panel (no bars) and no frame goes black while a resize
 * settles, BLEND materials show in every shading mode, each material slot renders its material,
 * the grid follows zoom and view, F frames the selection, and the nav gizmo / trackpad navigate.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"
#include <toyengine/render/toy_render_pipeline.h>

COOPA_TEST_SUITE("viewport");

namespace toy::editor::testing {

/**
 * @brief The viewport fills its panel: the render follows the panel's aspect (resolution_mode
 *        fill) through every rebuild a layout change triggers -- and no frame goes black while a
 *        rebuild settles: the UI's DrawList holds the pipeline's white texture from emit time, so
 *        a rebuild must not free it before that frame draws (it did; the frame showed black).
 */
COOPA_TEST(the_render_fills_the_panel_without_a_black_frame) {
    using coopa::input::Key;
    using coopa::input::Mods;
    EditorSession session({.settle_frames = 0});   // the startup rebuild is the first one checked
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto aspect_matches = [&](const std::string& what) {
        const imm::Box vb = app.viewport_box();
        const float panel = vb.w / std::max(1.0f, vb.h);
        const float render = float(engine.pipeline().render_width()) / float(engine.pipeline().render_height());
        expect(std::abs(panel - render) < 0.02f, what + ": render aspect " + std::to_string(render) +
                                                     " matches the panel's " + std::to_string(panel));
        const auto d = engine.display_rect();
        const float sc = std::max(1.0f, engine.display_scale());
        expect(std::abs(d.w - vb.w * sc) <= 2.0f && std::abs(d.h - vb.h * sc) <= 2.0f,
               what + ": the image covers the whole panel (no bars)");
    };
    // Share of the presented frame (UI included) that is pure black.
    auto black_share = [&]() {
        const coopa::gfx::util::ImageData img = engine.capture_image(false);
        size_t black = 0;
        for (size_t i = 0; i + 3 < img.pixels.size(); i += img.channels) {
            if (img.pixels[i] < 4 && img.pixels[i + 1] < 4 && img.pixels[i + 2] < 4) ++black;
        }
        return float(black) / float(std::max<size_t>(1, img.pixels.size() / img.channels));
    };
    auto settle_without_black = [&](const std::string& what) {
        const uint32_t w0 = engine.pipeline().render_width();
        float worst = 0.0f;
        for (int i = 0; i < toy::core::Engine::kFillDebounceFrames + 4; ++i) {
            engine.tick();
            worst = std::max(worst, black_share());
        }
        expect(engine.pipeline().render_width() != w0, what + ": the pipeline was rebuilt");
        expect(worst < 0.02f, what + ": no black frame during the rebuild (worst " +
                             std::to_string(int(worst * 100)) + "% black)");
    };

    // Startup: the first rebuild, from the whole window to the viewport panel.
    settle_without_black("startup");
    // A new project's config.yaml is the engine's own, so its vertical resolution is that one's.
    const uint32_t project_height = toy::core::AppConfig::load(session.project.config_path().string()).render.render_height;
    expect(engine.pipeline().render_height() == project_height, "the project's vertical resolution is kept");
    aspect_matches("default layout");

    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Space, Mods::Control);   // Blender's Toggle Maximize Area: a new viewport aspect
    settle_without_black("maximize");
    aspect_matches("maximized");
    dump(engine, "11_viewport_maximized");

    // The view still works after a rebuild: clicking the cube selects it.
    const ObjectId cube = session.named("cube");
    glm::vec2 px;
    if (engine.world_to_window(glm::vec3(0, 0, 0.5f), px)) {
        app.document().clear_selection();
        in.click(px / in.scale);
        tick(engine, 2);
        expect(app.document().primary() == cube, "picking works after the pipeline rebuild");
    }

    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Space, Mods::Control);   // and back
    settle_without_black("restore");
}

/** @brief BLEND materials are drawn in Solid / Material Preview (and the full render). */
COOPA_TEST(blend_materials_show_in_every_shading_mode) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});   // let the fill-mode rebuild settle
    auto& engine = session.engine;
    auto& app = session.app;
    const ObjectId cube = session.named("cube");
    expect(cube != 0, "the new project has a Cube");
    const int mr = app.document().find_component(cube, "MeshRenderer");
    auto set_alpha = [&](float a) {
        Node comp = app.document().find(cube)->at("components").as_seq()[static_cast<size_t>(mr)];
        Node mat = Node::mapping();
        mat["albedo"] = make_color({0.9f, 0.1f, 0.1f});
        mat["alpha_mode"] = Node(std::string("BLEND"));
        mat["alpha"] = make_float(a);
        comp["material"] = mat;
        app.document().set_component(cube, mr, comp, "Glass");
        app.sync().rebuild(engine, app.document());
        tick(engine, 4);
    };
    auto differing = [](const coopa::gfx::util::ImageData& a, const coopa::gfx::util::ImageData& b) {
        size_t n = 0;
        if (a.pixels.size() != b.pixels.size()) return size_t(0);
        for (size_t i = 0; i + 3 < a.pixels.size(); i += a.channels) {
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a.pixels[i + c]) - int(b.pixels[i + c])));
            if (d > 12) ++n;
        }
        return n;
    };
    for (Shading sh : {Shading::Solid, Shading::MaterialPreview, Shading::Full}) {
        app.set_shading(sh);
        set_alpha(0.0f);
        const auto clear = engine.capture_image(true);
        set_alpha(0.6f);
        const auto glass = engine.capture_image(true);
        const size_t n = differing(clear, glass);
        const char* name = sh == Shading::Solid ? "Solid" : sh == Shading::MaterialPreview ? "Material Preview" : "Rendered";
        expect(n > 200, std::string("a 60% alpha cube shows in ") + name + " (" + std::to_string(n) + " px changed)");
        if (sh == Shading::MaterialPreview) dump(engine, "10_glass_material_preview");
    }
}

/** @brief Submeshes end to end: a two-slot mesh draws each slot with its own material. */
COOPA_TEST(each_material_slot_renders_its_material) {
    // A cube with its top face in slot "trim"; a pure red material for it.
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4, .prepare = [](Project& project) {
        EditMesh m = make_cube();
        m.slots = {"body", "trim"};
        m.faces[1].slot = 1;
        coopa::yaml::save_document(project.assets() / "meshes" / "two_slot.yaml", mesh_to_node(m));
        Node red = Node::mapping();
        red["albedo"] = make_color({1.0f, 0.0f, 0.0f});
        red["emissive"] = make_color({1.0f, 0.0f, 0.0f});
        red["emissive_strength"] = make_float(2.0);
        coopa::yaml::save_document(project.assets() / "materials" / "red.yaml", red);
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    const ObjectId cube = session.named("cube");
    const int ci = app.document().find_component(cube, "MeshRenderer");
    Node comp = app.document().find(cube)->at("components").as_seq()[static_cast<size_t>(ci)];
    comp["mesh_path"] = Node(std::string("two_slot"));
    Node mats = Node::mapping();
    mats["trim"] = Node(std::string("materials/red"));
    comp["materials"] = mats;
    app.document().set_component(cube, ci, comp, "Slots");
    app.sync().rebuild(engine, app.document());
    tick(engine, 6);
    auto* mr = engine.scene().find_object("cube")->get_component<coopa::gfx::engine::components::MeshRenderer>();
    expect(mr && mr->is_ready() && mr->get_mesh()->part_count() == 2, "the renderer's mesh has two parts");
    expect(mr && mr->material_for(1).albedo.r > 0.99f && mr->material_for(0).albedo.r < 0.99f, "`materials: {trim: ...}` resolves by slot name");
    app.set_shading(Shading::MaterialPreview);
    tick(engine, 3);
    const auto img = engine.capture_image(true);
    size_t red = 0;
    for (size_t i = 0; i + 3 < img.pixels.size(); i += img.channels) {
        if (img.pixels[i] > 180 && img.pixels[i + 1] < 90 && img.pixels[i + 2] < 90) ++red;
    }
    expect(red > 30, "the trim slot renders red (" + std::to_string(red) + " px)");
    dump(engine, "18_submesh_materials");
}

/** @brief Blender's viewport grid: it faces the view down X / Y, its spacing follows the zoom,
 *         and F (or numpad .) frames the selection. */
COOPA_TEST(grid_follows_zoom_and_view_and_f_frames_the_selection) {
    using coopa::input::Key;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4,
                     .prepare = [](Project& p) { copy_rig_assets(p.assets()); }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm opens");
    tick(engine, 4);

    auto& cam = app.camera();
    cam.distance = 12.0f; cam.apply();
    expect(std::abs(app.grid_step() - 1.0f) < 1e-5f, "12 m out the grid is 1 m");
    cam.distance = 0.5f; cam.apply();
    expect(std::abs(app.grid_step() - 0.05f) < 1e-5f, "zoomed in to 0.5 m it is 5 cm");
    cam.distance = 300.0f; cam.apply();
    expect(std::abs(app.grid_step() - 50.0f) < 1e-3f, "300 m out it is 50 m");
    cam.distance = 12.0f; cam.apply();
    expect(app.grid_normal_axis() == 2, "an orbit view draws the floor grid");

    const ObjectId plate = object_named(app, "base_plate");
    const auto px = screen_of(engine, app, plate, glm::vec3(0.0f, -0.5f, 0.0f), in.scale);
    expect(px.has_value(), "base_plate is on screen");
    if (px) {
        in.click(*px);
        tick(engine, 2);
        expect(app.document().primary() == plate, "the click selects base_plate");
        const glm::vec3 framed = cam.focus;
        cam.focus = glm::vec3(40.0f, -30.0f, 5.0f);
        cam.apply();
        in.move(*px);
        tick(engine, 1);
        in.key(Key::F);
        tick(engine, 2);
        expect(glm::length(cam.focus - glm::vec3(40.0f, -30.0f, 5.0f)) > 10.0f && glm::length(cam.focus - framed) < 3.0f,
               "F refocuses the orbit on the selection");
        cam.focus = glm::vec3(40.0f, -30.0f, 5.0f);
        cam.apply();
        in.key(Key::Period);
        tick(engine, 2);
        expect(glm::length(cam.focus - framed) < 3.0f, ". frames the selection too");
    }

    in.key(Key::Kp1);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 1, "front view (numpad 1, looking down Y) draws the XZ grid");
    dump(engine, "22_grid_front");
    in.key(Key::Kp3);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 0, "right view (numpad 3, looking down X) draws the YZ grid");
    in.key(Key::Kp7);
    tick(engine, 2);
    expect(app.grid_normal_axis() == 2, "top view draws the floor grid");
}

/** @brief The nav gizmo's axis balls go orthographic along that axis (orbiting returns to
 *         perspective), and trackpad swipes orbit both ways while pinch zooms. */
COOPA_TEST(nav_axes_go_orthographic_and_trackpad_gestures_navigate) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& cam = app.camera();
    expect(!cam.ortho, "the view starts in perspective");

    // Click the +X ball: its screen spot is the view rotation applied to +X around the ball's centre.
    const coopa::ui::imm::Box g = app.nav_gizmo_rect();
    const glm::vec2 c{g.x + 55, g.y + 55};
    const glm::mat3 vr = glm::mat3(coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix());
    const glm::vec3 vx = vr * glm::vec3(1, 0, 0);
    in.click(c + glm::vec2(vx.x, -vx.y) * 40.0f);
    tick(engine, 2);
    expect(cam.ortho, "clicking the X ball goes orthographic");
    expect(std::abs(cam.forward().x + 1.0f) < 1e-3f, "and looks down the X axis (from +X)");

    auto scroll = [&](glm::vec2 d) {
        in.move(app.viewport_box().center());
        engine.queue_input([d](coopa::input::Input& i) { i.push_scroll(d.x, d.y); });
        tick(engine, 2);
    };
    // Trackpad: a vertical swipe tilts the view (and leaves the auto-ortho axis view).
    app.set_trackpad_for_test(true);
    const float pitch0 = cam.pitch_deg, dist0 = cam.distance;
    scroll({0.0f, 3.0f});
    expect(std::abs(cam.pitch_deg - pitch0) > 1.0f, "a vertical trackpad swipe tilts the view up / down");
    expect(std::abs(cam.distance - dist0) < 1e-4f, "...without zooming");
    expect(!cam.ortho, "orbiting out of the axis view returns to perspective");

    app.pinch_for_test(0.2);
    tick(engine, 2);
    expect(cam.distance < dist0 * 0.9f, "pinching out zooms in");

    // A wheel still zooms.
    app.set_trackpad_for_test(false);
    const float pitch1 = cam.pitch_deg, dist1 = cam.distance;
    scroll({0.0f, 1.0f});
    expect(cam.distance < dist1 && std::abs(cam.pitch_deg - pitch1) < 1e-4f, "a mouse wheel zooms, as before");

    // An explicit numpad 5 ortho stays ortho through orbits.
    cam.set_ortho(true);
    scroll({0.0f, 0.0f});
    app.set_trackpad_for_test(true);
    scroll({2.0f, 0.0f});
    expect(cam.ortho, "a chosen orthographic view stays orthographic when orbited");
    app.set_trackpad_for_test(std::nullopt);
}

} // namespace toy::editor::testing
