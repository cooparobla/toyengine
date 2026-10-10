/**
 * @file ui_designer_test.cpp
 * @brief The UI designer: a new UI asset opens with its canvas in the preview frame, widgets land in
 * the right parent, a click picks and a drag moves (one undo step), Interact runs the UI, the file
 * saves and places in a scene inside the viewer; templates expose the names their docs promise;
 * game UI themes preview unsaved edits and only Save writes them.
 */

#include <coopa/testing/test.h>

#include <uicoopa/binding/ui_handle.h>

#include "editor/support/editor_session.h"
#include "editor/ui/ui_canvas_math.h"

COOPA_TEST_SUITE("ui_designer");

namespace toy::editor::testing {

/**
 * @brief The UI designer end to end, through real input: a new UI asset opens in the designer
 *        with its canvas drawn inside the preview frame; widgets are added and land in the
 *        right parent; a click picks, a drag moves (one undo step); Interact runs the UI and
 *        echoes a button's click; the file saves; placed in a scene, the HUD stays inside the
 *        viewer.
 */
COOPA_TEST(design_pick_drag_interact_save_and_place) {
    using coopa::input::MouseButton;
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    const fs::path root = session.project.root();

    expect(app.new_ui_asset("test_hud", "blank"), "a blank UI asset is created");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::UI && app.ui_mode(), "it opens in the UI designer");
    expect(coopa::yaml::document_exists(project.assets() / "ui" / "test_hud.yaml"), "as assets/ui/test_hud.yaml");
    const imm::Box frame = app.ui_frame();
    expect(frame.w > 100 && frame.h > 50 && std::abs(frame.w / frame.h - 1920.0f / 1080.0f) < 0.02f,
           "the preview frame has the preview resolution's aspect");
    const auto placement = engine.scene_ui_placement();
    expect(placement && std::abs(static_cast<float>(placement->rect.w) - frame.w * in.scale) < 2.0f && !placement->input,
           "the engine places the asset's canvas in the frame (input off in Design)");

    // Add a window (into the root) and a menu into the window: the menu lands in its body.
    const ObjectId win = app.ui_add_widget("window");
    tick(engine, 3);
    expect(win && app.document().parent_of(win).value_or(0) == app.document().object_root(), "Add Window: a child of the canvas");
    const ObjectId menu = app.ui_add_widget("menu", win);
    tick(engine, 3);
    expect(menu && app.document().parent_of(menu).value_or(0) == win, "Add Menu List into the window");
    auto* menu_live = app.sync().live(menu);
    expect(menu_live && menu_live->parent() && menu_live->parent()->name() == "Body", "...and it sits in the Window's Body slot");
    auto wr = app.ui_live_rect(win);
    expect(wr && std::abs(wr->size().x - 440.0f) < 1.0f && std::abs(wr->size().y - 340.0f) < 1.0f, "the window is its authored 440 x 340");

    // The canvas draws: the window's panel colour is in the frame, the backdrop elsewhere.
    tick(engine, 2);
    const auto shot = engine.capture_image(false);
    const ui::UiView view = app.ui_view();
    const glm::vec2 wc = view.to_editor(wr->center() + glm::vec2(0, -40));
    auto px = [&](glm::vec2 e) {
        const uint32_t x = static_cast<uint32_t>(e.x * in.scale), y = static_cast<uint32_t>(e.y * in.scale);
        const size_t i = (static_cast<size_t>(y) * shot.width + x) * shot.channels;
        return glm::ivec3(shot.pixels[i], shot.pixels[i + 1], shot.pixels[i + 2]);
    };
    const glm::ivec3 inside = px(wc), backdrop = px({frame.x + 6, frame.y + 6});
    expect(glm::length(glm::vec3(inside - backdrop)) > 8.0f, "the window is drawn inside the frame (got " +
           std::to_string(inside.x) + "," + std::to_string(inside.y) + "," + std::to_string(inside.z) + " vs backdrop " +
           std::to_string(backdrop.x) + "," + std::to_string(backdrop.y) + "," + std::to_string(backdrop.z) + ")");
    dump(engine, "30_ui_designer");

    // Picking: the window's title bar picks the window (the menu covers its body).
    // (Left of centre: the middle of the top edge is the resize handle.)
    const glm::vec2 title_px = view.to_editor({wr->center().x - 120.0f, wr->max.y - 10.0f});
    const auto hits = app.ui_pick(title_px);
    expect(!hits.empty() && hits.front() == win, "a click on the title bar picks the window");

    // Real input: click selects; drag moves by the drag (one undo step).
    in.click(title_px);
    expect(app.document().primary() == win, "a real click on the window selects it");
    const glm::vec2 before = ui::parse_rect_block(*[&] {
        const Node* o = app.document().find(win);
        for (const auto& c : o->at("components").as_seq()) if (component_type(c) == "RectTransform") return &c;
        return static_cast<const Node*>(nullptr);
    }()).anchored_position();
    in.move(title_px);
    in.drag(title_px + glm::vec2(60.0f, 0.0f), MouseButton::Left, 6);
    tick(engine, 2);
    auto rect_of = [&](ObjectId id) {
        const Node* o = app.document().find(id);
        for (const auto& c : o->at("components").as_seq()) if (component_type(c) == "RectTransform") return ui::parse_rect_block(c);
        return coopa::ui::RectTransform{};
    };
    const glm::vec2 after = rect_of(win).anchored_position();
    const float expect_dx = 60.0f / view.scale;
    expect(std::abs((after.x - before.x) - expect_dx) < 2.0f && std::abs(after.y - before.y) < 1.5f,
           "dragging the window moves it by the drag (dx " + std::to_string(after.x - before.x) + ", want " + std::to_string(expect_dx) + ")");
    wr = app.ui_live_rect(win);
    expect(wr && std::abs(wr->center().x - 960.0f - after.x) < 2.0f, "...and the live rect follows without a rebuild");
    app.undo();
    tick(engine, 2);
    expect(glm::distance(rect_of(win).anchored_position(), before) < 1e-3f, "one Ctrl+Z undoes the whole drag");

    // Interact: the UI runs; clicking a menu button echoes its named click.
    app.set_ui_interact(true);
    tick(engine, 2);
    expect(app.ui_interacting() && engine.scene().is_simulating() && engine.scene_ui_placement()->input,
           "Interact runs the canvas and gives it input");
    coopa::scene::SceneObject* play_btn = app.sync().live(menu) ? app.sync().live(menu)->find_descendant("Play") : nullptr;
    auto* play_rt = play_btn ? play_btn->get_component<coopa::ui::RectTransform>() : nullptr;
    expect(play_rt != nullptr, "the menu built a button named Play");
    if (play_rt) {
        in.click(app.ui_view().to_editor(play_rt->rect().center()));
        tick(engine, 2);
        bool echoed = false;
        for (const auto& [lvl, line] : app.log()) echoed |= line.find("[UI] Play  click") != std::string::npos;
        expect(echoed, "clicking Play in Interact echoes its named click to the Console");
    }
    app.set_ui_interact(false);
    tick(engine, 3);
    expect(!engine.scene().is_simulating(), "back in Design the canvas stops");

    // Save: an object asset with the designer's preview settings riding along.
    expect(app.save_scene(), "the UI saves");
    const Node saved = coopa::yaml::load_document(project.assets() / "ui" / "test_hud.yaml");
    expect(saved.contains("object") && saved.contains("ui_editor") && saved.at("object").contains("children"),
           "saved as `object:` plus a `ui_editor:` block");

    // In a scene, the HUD draws inside the viewer -- not over the whole editor.
    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    const ObjectId inst = app.place_ui_asset("ui/test_hud.yaml");
    tick(engine, 4);
    expect(inst && get_string(*app.document().find(inst), "prefab") == "ui/test_hud", "the UI is placed as `prefab: ui/test_hud`");
    const auto sp = engine.scene_ui_placement();
    const auto dr = engine.display_rect();
    expect(sp && sp->rect.x == dr.x && sp->rect.w == dr.w && sp->rect.h == dr.h && !sp->input,
           "in the scene view the HUD is placed on the rendered image, not the whole window");
    auto* hud_live = engine.scene().find_object("Window");
    expect(hud_live != nullptr, "the placed HUD's window is in the live scene");
    dump(engine, "31_ui_in_scene");
}

/** @brief The shipped templates load, open, and expose the names their docs (and game code's
 *         bindings) promise. */
COOPA_TEST(templates_expose_the_names_their_docs_promise) {
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    const auto templates = EditorApp::ui_templates();
    expect(templates.size() >= 6, "UI templates ship with the editor (" + std::to_string(templates.size()) + ")");
    const std::map<std::string, std::vector<std::string>> names = {
        {"hud", {"Health", "Stamina", "Hotbar", "MessageLog"}},
        {"main_menu", {"NewGame", "Continue", "Settings", "Quit"}},
        {"pause_menu", {"Resume", "Settings", "QuitToMenu"}},
        {"dialog_box", {"Speaker", "Line", "Choices"}},
        {"inventory", {"Bag", "Equipment", "Gold"}},
        {"settings", {"MasterVolume", "Fullscreen", "Quality", "Apply"}},
    };
    for (const auto& t : templates) {
        expect(app.new_ui_asset(t, t), "template " + t + ": creates a UI asset");
        tick(engine, 4);
        expect(app.active_asset_type() == AssetType::UI && app.sync().scene(), "template " + t + ": opens in the designer");
        coopa::ui::UiHandle ui(app.sync().live(app.document().object_root()));
        auto it = names.find(t);
        if (it != names.end()) {
            for (const auto& n : it->second) expect(ui.has(n), "template " + t + ": has `" + n + "`");
        }
        dump(engine, "32_template_" + t);
    }
    expect(coopa::yaml::document_exists(project.assets() / "ui" / "themes" / "default.yaml"), "templates install their theme");
}

/**
 * @brief Game UI themes are assets: the Themes tab lists the shipped ones (default, Blender,
 *        Unity), opening one previews it on a UI with its unsaved edits -- in place of the UI's
 *        own theme -- and only Save writes the file.
 */
COOPA_TEST(theme_assets_preview_unsaved_edits_and_save) {
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    expect(app.new_ui_asset("menu", "settings"), "a UI asset to preview on");
    tick(engine, 4);
    const auto themes = app.list_assets(AssetType::Theme);
    for (const char* t : {"ui/themes/default.yaml", "ui/themes/blender.yaml", "ui/themes/unity.yaml"}) {
        expect(std::find(themes.begin(), themes.end(), t) != themes.end(), std::string("the Themes tab lists ") + t);
    }
    const auto uis = app.list_assets(AssetType::UI);
    expect(std::none_of(uis.begin(), uis.end(), [](const std::string& r) { return r.rfind("ui/themes/", 0) == 0; }),
           "themes are not UI assets");

    auto live_theme = [&]() -> const coopa::ui::UITheme* {
        auto* live = app.sync().live(app.document().object_root());
        auto* scope = live ? live->get_component<coopa::ui::ThemeScope>() : nullptr;
        return scope ? &scope->theme : nullptr;
    };
    const fs::path blender_file = project.assets() / "ui" / "themes" / "blender.yaml";
    const Node on_disk = coopa::yaml::load_document(blender_file);

    app.open_asset(AssetType::Theme, "ui/themes/blender.yaml");
    tick(engine, 4);
    expect(app.theme_document().open() && app.active_asset_type() == AssetType::UI, "a theme opens beside the UI it previews on");
    const auto* t = live_theme();
    const Node& bp = on_disk.at("panel").at("panel");
    expect(t && std::abs(t->panel.panel.r - get_float(bp, "r", -1.0f)) < 1e-4f,
           "the UI previews with the opened theme in place of its own (default.yaml)");
    dump(engine, "39_theme_blender");

    app.edit_theme("Panel", [](Node& n) {
        Node c = make_color({1.0f, 0.0f, 0.0f});
        c["a"] = make_float(1.0);
        n["panel"]["panel"] = c;
    });
    tick(engine, 4);
    t = live_theme();
    expect(t && t->panel.panel.r > 0.99f && t->panel.panel.g < 0.01f, "an unsaved edit shows in the preview");
    expect(coopa::yaml::load_document(blender_file) == on_disk && app.theme_document().dirty(), "...without touching the file");
    dump(engine, "40_theme_preview");

    app.theme_document().do_undo();
    expect(app.theme_document().node == on_disk, "theme edits undo");
    app.theme_document().do_redo();
    expect(app.save_theme() && !app.theme_document().dirty(), "Save Theme writes it");
    expect(get_float(coopa::yaml::load_document(blender_file).at("panel").at("panel"), "g", -1.0f) < 0.01f, "the file has the edit");

    // Another theme replaces it in the preview.
    app.open_asset(AssetType::Theme, "ui/themes/unity.yaml");
    tick(engine, 4);
    const Node unity_doc = coopa::yaml::load_document(project.assets() / "ui" / "themes" / "unity.yaml");
    const Node& up = unity_doc.at("panel").at("panel");
    t = live_theme();
    expect(app.theme_document().ref == "ui/themes/unity" && t && std::abs(t->panel.panel.r - get_float(up, "r", -1.0f)) < 1e-4f,
           "opening another theme previews that one (" + app.theme_document().ref + ", panel.r " +
           std::to_string(t ? t->panel.panel.r : -1.0f) + " vs " + std::to_string(get_float(up, "r", -1.0f)) + ")");
    dump(engine, "41_theme_unity");
}

} // namespace toy::editor::testing
