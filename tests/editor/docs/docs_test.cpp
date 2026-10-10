/**
 * @file docs_test.cpp
 * @brief The editor user manual's screenshots (docs/editor/, the editor-user-docs skill) -- a
 *        tool, not a test: built into toyengine_editor_docs, never registered with ctest.
 *
 * @code
 * DOCS_SHOT_DIR=/path/to/out ./build/tests/toyengine_editor_docs --suite docs [docs/<shot>]
 * @endcode
 *
 * Each test stages one clean, user-representative editor state against a scratch copy of this
 * repo's assets/ (named my_game, like the manual's examples) and saves <shot>.png at display
 * resolution. Without DOCS_SHOT_DIR a shot fails at once. One test per shot, so a flaky run can
 * be retried by name.
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("docs");

namespace toy::editor::testing {
namespace {

const char* docs_shot_dir() {
    const char* d = std::getenv("DOCS_SHOT_DIR");
    return d && *d ? d : nullptr;
}

/** @brief A shot needs somewhere to go: DOCS_SHOT_DIR, or the test fails at once. */
void require_shot_dir() { coopa::test::require(docs_shot_dir() != nullptr, "DOCS_SHOT_DIR names the output folder"); }

/** @brief The editor as a user sees it: 1600 x 900, full resolution, no pixel-art post effects. */
toy::core::AppConfig docs_config(const Project& p) {
    install_codecs_once();
    toy::core::AppConfig cfg = toy::core::AppConfig::load(p.config_path().string());
    cfg.window.width = 1600;
    cfg.window.height = 900;
    cfg.window.visible = false;
    cfg.window.vsync = false;
    cfg.render.screen_ui_enabled = true;
    cfg.render.outline_enabled = false;
    cfg.render.palette_enabled = false;
    cfg.render.dither_enabled = false;
    cfg.output.save_on_exit = false;
    apply_editor_render_overrides(cfg);
    return cfg;
}

/** @brief A scratch project: a copy of this repo's assets/ (unless `fresh`, which is exactly what
 *         a new project starts with), plus the starter scenes/main. */
Project docs_project(const std::string& shot, bool fresh = false) {
    use_scratch_home();   // preferences start from the defaults (theme included) and stay in scratch
    const fs::path root = coopa::test::scratch_dir("docs_" + shot) / "my_game";
    fs::create_directories(root);
    if (!fresh) fs::copy(fs::path(ROOT_DIR) / "assets", root / "assets", fs::copy_options::recursive);
    return Project::create(root);   // adds only what is missing: the starter scenes/main
}

/** @brief One editor session staged for one screenshot. */
struct DocsEditor {
    std::string shot;
    Project project;
    std::unique_ptr<toy::core::Engine> engine;
    std::unique_ptr<EditorApp> app;
    std::unique_ptr<InputDriver> in;
    std::chrono::steady_clock::time_point cleared;

    DocsEditor(const std::string& name, const std::string& scene_rel, const std::string& theme = {},
               bool fresh_project = false)
        : shot(name), project(docs_project(name, fresh_project)) {
        setenv("FIXED_DT", "0.016666", 1);
        setenv("NO_INPUT", "1", 1);
        if (!theme.empty()) {
            Node prefs = Node::mapping();
            prefs["theme"] = Node(theme);
            Project::save_prefs(prefs);
        }
        engine = std::make_unique<toy::core::Engine>(docs_config(project), shell_options(project));
        app = std::make_unique<EditorApp>(*engine, project, project.assets() / scene_rel);
        in = std::make_unique<InputDriver>(InputDriver{*engine, std::max(1.0f, engine->display_scale())});
        tick(*engine, toy::core::Engine::kFillDebounceFrames + 6);
        // A window the OS shrank to fit a smaller screen lays the editor out differently.
        const glm::vec2 canvas = app->ui().canvas_size();
        if (std::abs(canvas.x - 1600.0f) > 0.5f || std::abs(canvas.y - 900.0f) > 0.5f) {
            throw std::runtime_error("the editor window is " + std::to_string(int(canvas.x)) + " x " +
                                     std::to_string(int(canvas.y)) + ", not 1600 x 900 (retry)");
        }
        // The starter scene is written without object ids; loading stamps them (unsaved).
        // A user's project has been saved: so is this one.
        if (app->document().dirty()) app->save_scene();
        tick(*engine, 2);
        clear_console();
    }
    ~DocsEditor() {
        app.reset();
        engine.reset();
    }
    EditorApp& a() { return *app; }
    toy::core::Engine& e() { return *engine; }
    imm::Box vb() { return app->viewport_box(); }

    /** @brief Clicks the Console's Clear (trash) button: the opening messages name scratch paths. */
    void clear_console() {
        const imm::Box v = vb();
        in->click({v.right() - 22.0f, v.bottom() + 14.0f});
        rest();
        cleared = std::chrono::steady_clock::now();
    }
    /** @brief Parks the mouse where it hovers nothing (the viewport's lower right). */
    void rest() {
        const imm::Box v = vb();
        in->move({v.right() - 160.0f, v.bottom() - 60.0f}, 2);
    }
    /** @brief Centre of a top-bar menu header (0 File, 1 Edit, 2 Render, 3 Window, 4 Help). */
    glm::vec2 topbar_menu(int index) {
        static const char* menus[] = {"File", "Edit", "Render", "Window", "Help"};
        auto& ctx = app->ui();
        float x = 28.0f + 4.0f;
        for (int i = 0; i < index; ++i) x += ctx.text_width(menus[i]) + ctx.style.padding * 3;
        return {x + (ctx.text_width(menus[index]) + ctx.style.padding * 3) * 0.5f, 14.0f};
    }
    /** @brief Centre of an item in a drop-down opened at `top_left`: `rows` items and `seps` separators above it. */
    glm::vec2 menu_row(glm::vec2 top_left, int rows, int seps) {
        auto& st = app->ui().style;
        const float y = top_left.y + 4.0f + rows * (st.row_height + st.spacing) + seps * (5.0f + st.spacing) + st.row_height * 0.5f;
        return {top_left.x + 70.0f, y};
    }
    /** @brief Saves <shot>.png (or <shot><suffix>.png) once the status bar's opening message has timed out. */
    void capture(const std::string& name = {}) {
        // The status bar repeats the latest Console message for 8 s (wall clock). With the
        // Console cleared, the latest is still the opening "Opened project <scratch path>".
        if (app->log().empty()) {
            const auto until = cleared + std::chrono::milliseconds(8300);
            while (std::chrono::steady_clock::now() < until) tick(*engine, 1);
        }
        tick(*engine, 3);
        for (const auto& [lvl, line] : app->log()) std::cout << "    console: " << line << "\n";
        const fs::path out = fs::path(docs_shot_dir()) / ((name.empty() ? shot : name) + ".png");
        fs::create_directories(out.parent_path());
        engine->save_screenshot(out.string(), false);
        std::cout << "    saved " << out.string() << "\n";
    }
};

/** @brief Numpad 0: the view through the scene's own camera (how the scene author framed it). */
void docs_scene_camera(DocsEditor& d) {
    d.in->move(d.vb().center());
    d.in->key(coopa::input::Key::Kp0);
    d.rest();
}

/** @brief A closer orbit view of the starter scene's cube. */
void docs_main_view(DocsEditor& d, float distance = 7.0f) {
    auto& cam = d.a().camera();
    cam.focus = glm::vec3(0.0f, 0.0f, 0.6f);
    cam.yaw_deg = 35.0f;
    cam.pitch_deg = 28.0f;
    cam.distance = distance;
    cam.apply();
    tick(d.e(), 2);
}

/** @brief Selects the move tool in the viewport toolbar (its gizmo then shows on the selection). */
void docs_pick_move_tool(DocsEditor& d) {
    const imm::Box v = d.vb();
    d.in->click({v.x + 25.0f, v.y + 100.0f});
}

} // namespace

// 1. The water_demo scene in Rendered shading, an object selected with its gizmo, Object tab.
COOPA_TEST(overview) {
    require_shot_dir();
    DocsEditor d("overview", "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    const ObjectId boat = object_named(d.a(), "boat");
    expect(boat != 0, "water_demo has the boat");
    docs_pick_move_tool(d);
    d.a().document().select(boat);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// The README's editor shot: fog_demo in Full Render, a stage spotlight selected.
COOPA_TEST(readme_editor) {
    require_shot_dir();
    DocsEditor d("readme_editor", "scenes/rendering/fog_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    const ObjectId spot = object_named(d.a(), "stage_spot_left");
    expect(spot != 0, "fog_demo has the stage spotlight");
    docs_pick_move_tool(d);
    d.a().document().select(spot);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 90);
    d.capture();
}

// The README's Getting started shot: a brand-new project's starter scene in Full Render.
COOPA_TEST(getting_started_editor) {
    require_shot_dir();
    DocsEditor d("getting_started_editor", "scenes/main/scene.yaml", {}, /*fresh_project=*/true);
    d.a().set_shading(Shading::Full);
    docs_main_view(d, 9.0f);
    const ObjectId cube = object_named(d.a(), "cube");
    expect(cube != 0, "the starter scene has its cube");
    docs_pick_move_tool(d);
    d.a().document().select(cube);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 2. File menu open.
COOPA_TEST(menu_file) {
    require_shot_dir();
    DocsEditor d("menu_file", "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    d.in->click(d.topbar_menu(0));
    d.in->move(d.menu_row({32.0f, 27.0f}, 3, 1), 2);   // hover Save
    expect(d.a().ui().any_popup_open(), "the File menu is open");
    d.capture();
}

// 3. An object with several components selected; its components in Properties.
COOPA_TEST(hierarchy_inspector) {
    require_shot_dir();
    DocsEditor d("hierarchy_inspector", "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    // Click lake_water's Hierarchy row, then its arrow to list its components.
    const float row_y = 119.0f, right_x = d.vb().right() + 1.0f;
    d.in->click({right_x + 90.0f, row_y});
    d.in->click({right_x + 31.0f, row_y});
    expect(d.a().document().primary() == object_named(d.a(), "lake_water"), "lake_water is selected");
    d.a().set_prop_tab(PropTab::Components);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 4. The Add Component menu open.
COOPA_TEST(add_component) {
    require_shot_dir();
    DocsEditor d("add_component", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.a().set_prop_tab(PropTab::Components);
    tick(d.e(), 4);
    // Find the button on screen: the lowest block of the theme's button colour in the
    // Properties column (Add Component follows the last component panel).
    const glm::vec4 bc = d.a().ui().style.button;
    const auto img = d.e().capture_image(false);
    const float sc = d.in->scale;
    const int x = static_cast<int>((d.vb().right() + 46.0f) * sc);
    int run = 0, best_mid = -1;
    for (int y = 0; y < static_cast<int>(img.height); ++y) {
        const uint8_t* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * img.channels];
        bool match = true;
        for (int c = 0; c < 3; ++c) match &= std::abs(int(q[c]) - int(std::lround(bc[c] * 255.0f))) <= 4;
        run = match ? run + 1 : 0;
        if (run >= static_cast<int>(14 * sc)) best_mid = y - static_cast<int>(7 * sc);
    }
    expect(best_mid >= 0, "the Add Component button is on screen");
    const glm::vec2 button{d.vb().right() + 46.0f, best_mid / sc};
    d.in->click(button);
    expect(d.a().ui().any_popup_open(), "Add Component opens its menu");
    // Type into its search box: "light" narrows the list to the lights. The full list is
    // taller than the window, so the menu sits against the top edge until it is filtered.
    d.in->click({button.x - 46.0f + 1.0f + 100.0f, 14.0f});
    for (char ch : std::string("light")) {
        d.e().queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
        tick(d.e(), 1);
    }
    d.in->key(coopa::input::Key::Enter);   // the search field applies on Enter
    tick(d.e(), 3);
    d.in->move(button + glm::vec2(30.0f, 123.0f), 3);   // hover PointLight
    tick(d.e(), 20);
    d.capture();
}

// 5. Solid shading with the Viewport Shading popover open.
COOPA_TEST(viewport_solid) {
    require_shot_dir();
    DocsEditor d("viewport_solid", "scenes/main/scene.yaml");
    docs_main_view(d);
    d.a().set_shading(Shading::Solid);
    tick(d.e(), 10);
    const imm::Box v = d.vb();
    d.in->click({v.right() - 13.0f, v.y - 13.0f});
    expect(d.a().ui().any_popup_open(), "the Viewport Shading popover is open");
    d.in->move({v.right() - 120.0f, v.y + 160.0f}, 2);
    d.capture();
}

// 6. Mid G-move constrained to X, with the axis line.
COOPA_TEST(gizmo_move) {
    require_shot_dir();
    using coopa::input::Key;
    DocsEditor d("gizmo_move", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    docs_pick_move_tool(d);
    d.a().document().select(cube);
    tick(d.e(), 4);
    const auto c = screen_of(d.e(), d.a(), cube, glm::vec3(0.0f), d.in->scale);
    const glm::vec2 start = c ? *c : d.vb().center();
    d.in->move(start + glm::vec2(40.0f, 0.0f));
    d.in->key(Key::G);
    d.in->key(Key::X);
    for (int i = 1; i <= 8; ++i) d.in->move(start + glm::vec2(40.0f + 12.0f * i, -4.0f * i));
    expect(d.a().modal_active(), "the move is in progress");
    tick(d.e(), 4);
    d.capture();
}

void docs_stage_edit_mode(DocsEditor& d);

// 7. X-Ray in Edit Mode.
COOPA_TEST(xray_edit) {
    require_shot_dir();
    using coopa::input::Key;
    using coopa::input::Mods;
    DocsEditor d("xray_edit", "scenes/main/scene.yaml");
    docs_stage_edit_mode(d);
    d.in->move(d.vb().center());
    d.in->key(Key::Num1);
    d.in->key(Key::A);
    d.in->key(Key::Z, Mods::Alt);
    expect(d.a().edit_mode_active() && d.e().render_config().editor_xray_alpha < 0.99f, "X-Ray is on in Edit Mode");
    d.rest();
    tick(d.e(), 10);
    d.capture();
}

// The Colliders toggle on: weather_demo's colliders (houses, roofs, trees) as green wireframes, the well selected.
COOPA_TEST(viewport_colliders) {
    require_shot_dir();
    DocsEditor d("viewport_colliders", "scenes/effects/weather_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    const ObjectId well = object_named(d.a(), "well");
    expect(well != 0, "weather_demo has its well");
    d.a().document().select(well);
    d.a().set_show_colliders(true);
    d.rest();
    tick(d.e(), 60);
    expect(d.a().collider_lines_drawn() > 0, "the colliders draw");
    d.capture();
}

/** @brief Edit Mode on the starter cube, face select, a loop cut and an extruded top face. */
void docs_stage_edit_mode(DocsEditor& d) {
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::Tab);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    expect(d.a().edit_mode_active(), "Tab enters Edit Mode");
    // Ctrl+R on a vertical edge: one loop around the middle.
    const glm::vec2 corner = visible_corner(d.a(), cube);
    if (auto edge = screen_of(d.e(), d.a(), cube, glm::vec3(corner, 0.1f), d.in->scale)) {
        d.in->move(*edge);
        d.in->key(Key::R, Mods::Control);
        d.in->move(*edge + glm::vec2(1, 0));
        d.in->click(*edge);
        d.in->click(*edge, MouseButton::Right);
    }
    // Face select, the top face, E 0.6 Enter.
    d.in->key(Key::Num3);
    auto& md = d.a().mesh_document();
    md.selection.faces.clear();
    for (uint32_t f = 0; f < md.mesh.faces.size(); ++f) {
        if (md.mesh.face_center(f).z > 0.49f) md.selection.faces.insert(f);
    }
    d.in->move(d.vb().center());
    d.in->key(Key::E);
    d.in->key(Key::Period);
    d.in->key(Key::Num6);
    d.in->key(Key::Enter);
    // Step back to see the whole (now taller) mesh.
    auto& cam = d.a().camera();
    cam.focus = glm::vec3(0.0f, 0.0f, 0.75f);
    cam.distance = 4.6f;
    cam.apply();
    tick(d.e(), 4);
}

// 8. Edit Mode, face select, faces selected after an extrude.
COOPA_TEST(edit_mode) {
    require_shot_dir();
    DocsEditor d("edit_mode", "scenes/main/scene.yaml");
    docs_stage_edit_mode(d);
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

// 9. The Adjust Last Operation panel after a loop cut.
COOPA_TEST(adjust_last_operation) {
    require_shot_dir();
    using coopa::input::Key;
    using coopa::input::Mods;
    using coopa::input::MouseButton;
    DocsEditor d("adjust_last_operation", "scenes/main/scene.yaml");
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::Tab);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    d.in->key(Key::Num2);
    const glm::vec2 corner = visible_corner(d.a(), cube);
    if (auto edge = screen_of(d.e(), d.a(), cube, glm::vec3(corner, 0.1f), d.in->scale)) {
        d.in->move(*edge);
        d.in->key(Key::R, Mods::Control);
        d.in->move(*edge + glm::vec2(1, 0));
        d.in->click(*edge);
        d.in->click(*edge, MouseButton::Right);
    }
    expect(d.a().mesh_document().mesh.faces.size() > 6, "the loop cut ran");
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

/** @brief A sphere added to the starter scene, selected, in `mode`, smoothed for the brushes. */
ObjectId docs_brush_sphere(DocsEditor& d, InteractionMode mode) {
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.a().delete_selected();
    tick(d.e(), 2);
    const ObjectId sphere = d.a().create_primitive("Sphere");
    tick(d.e(), 3);
    d.a().document().set_transform(sphere, {0, 0, 1}, glm::vec3(0), glm::vec3(1), "Move");
    d.a().sync().apply(d.e(), d.a().document(), {ChangeScope::Transform, sphere});
    d.a().document().select(sphere);
    d.a().frame_selected();
    tick(d.e(), 4);
    d.a().set_interaction_mode(mode);
    tick(d.e(), toy::core::Engine::kFillDebounceFrames + 2);
    d.a().subdivide_smooth(1);
    tick(d.e(), 3);
    return sphere;
}

/** @brief A brush stroke across the sphere (local points on its surface, in screen space). */
void docs_stroke(DocsEditor& d, ObjectId id, glm::vec3 a, glm::vec3 b) {
    auto p0 = screen_of(d.e(), d.a(), id, a, d.in->scale);
    auto p1 = screen_of(d.e(), d.a(), id, b, d.in->scale);
    if (!p0 || !p1) return;
    d.in->move(*p0);
    d.in->drag(*p1, coopa::input::MouseButton::Left, 14);
    tick(d.e(), 2);
}

/** @brief The sphere's camera-facing side: local points for strokes. */
glm::vec3 docs_front(DocsEditor& d, float u, float v) {
    const glm::mat4 view = coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix();
    const glm::mat3 inv = glm::transpose(glm::mat3(view));
    const glm::vec3 right = inv[0], up = inv[1], back = inv[2];
    return glm::normalize(back + right * u + up * v) * 0.98f;
}

// 10. Sculpt Mode after a few strokes, the brush circle under the mouse.
COOPA_TEST(sculpt) {
    require_shot_dir();
    DocsEditor d("sculpt", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::Sculpt);
    expect(d.a().interaction_mode() == InteractionMode::Sculpt, "Sculpt Mode");
    d.a().sculpt_settings().radius_px = 35.0f;
    d.a().sculpt_settings().strength = 0.45f;
    docs_stroke(d, s, docs_front(d, -0.6f, 0.3f), docs_front(d, 0.5f, 0.35f));
    docs_stroke(d, s, docs_front(d, -0.5f, -0.2f), docs_front(d, 0.4f, -0.3f));
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.15f, 0.05f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 11. Vertex Paint with painted colours.
COOPA_TEST(vertex_paint) {
    require_shot_dir();
    DocsEditor d("vertex_paint", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::VertexPaint);
    expect(d.a().interaction_mode() == InteractionMode::VertexPaint, "Vertex Paint");
    auto& vs = d.a().vertex_paint_settings();
    vs.radius_px = 40.0f;
    vs.color = glm::vec4(0.9f, 0.15f, 0.1f, 1.0f);
    docs_stroke(d, s, docs_front(d, -0.6f, 0.4f), docs_front(d, 0.6f, 0.4f));
    vs.color = glm::vec4(0.15f, 0.5f, 0.95f, 1.0f);
    docs_stroke(d, s, docs_front(d, -0.6f, -0.3f), docs_front(d, 0.6f, -0.3f));
    vs.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f);
    docs_stroke(d, s, docs_front(d, 0.0f, 0.6f), docs_front(d, 0.0f, -0.6f));
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.3f, 0.05f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 12. Weight Paint heatmap with a vertex group.
COOPA_TEST(weight_paint) {
    require_shot_dir();
    DocsEditor d("weight_paint", "scenes/main/scene.yaml");
    const ObjectId s = docs_brush_sphere(d, InteractionMode::WeightPaint);
    expect(d.a().interaction_mode() == InteractionMode::WeightPaint, "Weight Paint");
    auto& ws = d.a().weight_paint_settings();
    ws.radius_px = 55.0f;
    ws.weight = 1.0f;
    docs_stroke(d, s, docs_front(d, -0.7f, 0.5f), docs_front(d, 0.7f, 0.5f));
    docs_stroke(d, s, docs_front(d, -0.5f, 0.2f), docs_front(d, 0.5f, 0.2f));
    ws.weight = 0.5f;
    docs_stroke(d, s, docs_front(d, -0.6f, -0.2f), docs_front(d, 0.6f, -0.2f));
    expect(!d.a().mesh_document().mesh.groups.empty(), "a vertex group was painted");
    if (auto p = screen_of(d.e(), d.a(), s, docs_front(d, 0.35f, -0.45f), d.in->scale)) d.in->move(*p, 3);
    tick(d.e(), 4);
    d.capture();
}

// 13. A material in the material editor, on the shader-ball lookdev.
COOPA_TEST(material_editor) {
    require_shot_dir();
    DocsEditor d("material_editor", "scenes/main/scene.yaml");
    d.in->click({18.0f + 25.0f * 3.0f, 44.0f});   // the Materials tab
    d.a().open_asset(AssetType::Material, "materials/brick.yaml");
    tick(d.e(), 6);
    expect(d.a().active_asset_type() == AssetType::Material, "the material opens");
    tick(d.e(), 60);
    d.rest();
    d.capture();
}

/** @brief Opens the robot arm object asset, the joint `elbow` selected. */
ObjectId docs_robot_arm(DocsEditor& d) {
    d.in->click({18.0f + 25.0f, 44.0f});   // the Objects tab
    d.a().open_asset(AssetType::Object, "objects/robot_arm.yaml");
    tick(d.e(), 8);
    expect(d.a().active_asset_type() == AssetType::Object, "the robot arm opens");
    return object_named(d.a(), "elbow");
}

// 14. The Timeline dope sheet with keys.
COOPA_TEST(timeline) {
    require_shot_dir();
    DocsEditor d("timeline", "scenes/main/scene.yaml");
    const ObjectId elbow = docs_robot_arm(d);
    d.a().show_timeline();
    d.a().document().select(elbow);
    tick(d.e(), 3);
    d.a().set_animation_time(0.6f);
    expect(d.a().animation_clip() != nullptr, "the arm's clip is open");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

// 15. Record on: the red viewport frame.
COOPA_TEST(record_autokey) {
    require_shot_dir();
    DocsEditor d("record_autokey", "scenes/main/scene.yaml");
    const ObjectId elbow = docs_robot_arm(d);
    d.a().show_timeline();
    d.a().document().select(elbow);
    tick(d.e(), 3);
    d.a().set_animation_time(0.4f);
    d.a().set_animation_record(true);
    expect(d.a().animation_record(), "Record is on");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

// 16. An object asset open.
COOPA_TEST(object_asset) {
    require_shot_dir();
    DocsEditor d("object_asset", "scenes/main/scene.yaml");
    docs_robot_arm(d);
    d.in->click({d.vb().right() + 15.0f, 71.0f});   // expand the root's Hierarchy row
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

/** @brief Opens ui/<name>.yaml in the UI designer. */
void docs_open_ui(DocsEditor& d, const std::string& name) {
    d.a().open_asset(AssetType::UI, "ui/" + name + ".yaml");
    tick(d.e(), 8);
    expect(d.a().ui_mode(), "ui/" + name + " opens in the UI designer");
}

// 17. The HUD in the designer, an element selected (rect gizmo), Element tab.
COOPA_TEST(ui_designer) {
    require_shot_dir();
    DocsEditor d("ui_designer", "scenes/main/scene.yaml");
    docs_open_ui(d, "hud");
    d.in->click({d.vb().right() + 15.0f, 71.0f});   // expand the root's Hierarchy row
    const ObjectId hotbar = object_named(d.a(), "Hotbar");
    expect(hotbar != 0, "the HUD has a Hotbar");
    d.a().document().select(hotbar);
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 10);
    d.capture();
}

// 18. The UI tab's + menu (New UI) listing the templates.
COOPA_TEST(ui_new_menu) {
    require_shot_dir();
    DocsEditor d("ui_new_menu", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.in->click({242.0f, 44.0f});
    expect(d.a().ui().any_popup_open(), "+ opens the New UI menu");
    d.in->move({300.0f, 400.0f}, 2);
    d.capture();
}

// 19. Interact mode running.
COOPA_TEST(ui_interact) {
    require_shot_dir();
    DocsEditor d("ui_interact", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.a().set_ui_interact(true);
    tick(d.e(), 10);
    expect(d.a().ui_interacting(), "Interact is running");
    // Click Continue (its click is echoed to the Console), then hover Settings.
    auto button = [&](const char* name) -> std::optional<glm::vec2> {
        auto* live = d.a().sync().live(d.a().document().object_root());
        auto* b = live ? live->find_descendant(name) : nullptr;
        auto* rt = b ? b->get_component<coopa::ui::RectTransform>() : nullptr;
        if (!rt) return std::nullopt;
        return d.a().ui_view().to_editor(rt->rect().center());
    };
    if (auto c = button("Continue")) d.in->click(*c);
    if (auto st = button("Settings")) d.in->move(*st, 4);
    tick(d.e(), 10);
    d.capture();
}

// 20. The Bindings tab.
COOPA_TEST(ui_bindings) {
    require_shot_dir();
    DocsEditor d("ui_bindings", "scenes/main/scene.yaml");
    docs_open_ui(d, "main_menu");
    d.a().set_prop_tab(PropTab::Bindings);
    d.rest();
    tick(d.e(), 6);
    d.capture();
}

// 21 / 22. Render and World properties.
void docs_settings_tab(const std::string& shot, PropTab tab) {
    DocsEditor d(shot, "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    d.a().document().clear_selection();
    d.a().set_prop_tab(tab);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}
COOPA_TEST(render_settings) {
    require_shot_dir();
    docs_settings_tab("render_settings", PropTab::Render);
}
COOPA_TEST(world_settings) {
    require_shot_dir();
    docs_settings_tab("world_settings", PropTab::World);
}

// 23. File > Package Project (.caml)... open.
COOPA_TEST(package_dialog) {
    require_shot_dir();
    DocsEditor d("package_dialog", "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    d.in->click(d.topbar_menu(0));
    d.in->click(d.menu_row({32.0f, 27.0f}, 8, 3));
    tick(d.e(), 3);
    expect(d.a().ui().is_popup_open("Package Project"), "the Package Project window is open");
    // The default output folder is <project>/build/package; this project lives in a scratch
    // folder, so show it as it reads for a project at ~/my_game (typed into the field).
    d.in->click({540.0f + 380.0f, 345.0f + 69.0f});   // the Output folder field (520 x 210 window, centred)
    for (char ch : std::string("~/my_game/build/package")) {
        d.e().queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
        tick(d.e(), 1);
    }
    d.in->key(coopa::input::Key::Enter);
    d.rest();
    d.capture();
}

// 24. Blender Light theme, overview-like.
COOPA_TEST(theme_light) {
    require_shot_dir();
    DocsEditor d("theme_light", "scenes/water/water_demo/scene.yaml", "blender_light");
    expect(d.a().theme_id() == "blender_light", "Blender Light is active");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    docs_pick_move_tool(d);
    d.a().document().select(object_named(d.a(), "boat"));
    d.a().set_prop_tab(PropTab::Object);
    d.rest();
    tick(d.e(), 60);
    d.capture();
}

// 25. Help > Controls... open.
COOPA_TEST(controls_modal) {
    require_shot_dir();
    DocsEditor d("controls_modal", "scenes/water/water_demo/scene.yaml");
    d.a().set_shading(Shading::Full);
    docs_scene_camera(d);
    tick(d.e(), 40);
    const glm::vec2 help = d.topbar_menu(4);
    d.in->click(help);
    const float hx = help.x - (d.a().ui().text_width("Help") + d.a().ui().style.padding * 3) * 0.5f;
    d.in->click(d.menu_row({hx, 27.0f}, 0, 0));
    tick(d.e(), 3);
    expect(d.a().ui().is_popup_open("Controls"), "the Controls window is open");
    d.rest();
    d.capture();
}

// 26. The Unsaved Changes prompt.
COOPA_TEST(unsaved_prompt) {
    require_shot_dir();
    using coopa::input::Key;
    DocsEditor d("unsaved_prompt", "scenes/main/scene.yaml");
    docs_main_view(d);
    const ObjectId cube = object_named(d.a(), "cube");
    d.a().document().select(cube);
    d.in->move(d.vb().center());
    d.in->key(Key::G);
    d.in->key(Key::X);
    d.in->key(Key::Num2);
    d.in->key(Key::Enter);
    d.a().open_asset(AssetType::Material, "materials/brick.yaml");
    tick(d.e(), 4);
    expect(d.a().ui().is_popup_open("Unsaved Changes"), "switching asks to save first");
    d.rest();
    d.capture();
}

// 27. The Asset panel's Meshes tab, a mesh open in the mesh viewer.
COOPA_TEST(asset_panel_meshes) {
    require_shot_dir();
    DocsEditor d("asset_panel_meshes", "scenes/main/scene.yaml");
    d.in->click({18.0f + 25.0f * 2.0f, 44.0f});   // the Meshes tab
    d.a().open_asset(AssetType::Mesh, "meshes/barrel.yaml");
    tick(d.e(), 8);
    expect(d.a().active_asset_type() == AssetType::Mesh, "the mesh opens in the mesh viewer");
    d.rest();
    tick(d.e(), 30);
    d.capture();
}

} // namespace toy::editor::testing
