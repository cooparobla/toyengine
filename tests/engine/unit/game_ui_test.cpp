/**
 * @file game_ui_test.cpp
 * @brief Authored game UI (uicoopa through the engine's parsers, no device): composites expand
 *        from YAML and slot authored children, UiHandle binds by name (signals, bars, dialogs),
 *        themes scope to their subtree and the shipped ones load, placed canvases map scissor and
 *        cursor, and a UI prefab's own RectTransform replaces the asset's.
 *
 * Not here: how a theme LOOKS (colours, metrics) -- that is design, not a contract.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <fkYAML/node.hpp>
#include <coopa/scene/scene_inherit.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/yaml/document.h>
#include <uicoopa/ui_yaml.h>
#include <toyengine/ui/ui_assets.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("game_ui");

using coopa::scene::Scene;
using coopa::scene::SceneObject;

namespace {

/** @brief A started scene parsed from `yaml` with the UI parsers (headless) registered. */
Scene load_ui_scene(const std::string& yaml, const std::string& anchor = "ui_test.yaml") {
    coopa::ui::register_ui_components();
    Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(yaml), anchor);
    return scene;
}

const char* kCompositeScene = R"(
scene:
  scene_name: ui_test
  root_objects:
    - name: hud
      components:
        - {type: RectTransform, anchor_preset: StretchAll, size_delta: {x: 0, y: 0}}
        - {type: Canvas, reference_resolution: {x: 1280, y: 720}}
      children:
        - name: Inventory
          components:
            - {type: RectTransform, anchor_preset: MiddleCenter, size_delta: {x: 400, y: 300}}
            - {type: Window, title: Bag, close_button: true}
          children:
            - name: Hello
              components:
                - {type: RectTransform, size_delta: {x: 100, y: 20}}
                - {type: Text, text: hi}
            - name: Volume
              components:
                - {type: RectTransform, size_delta: {x: 300, y: 28}}
                - {type: SettingRow, kind: slider, label: Volume, min: 0, max: 10, value: 4}
        - name: Menu
          components:
            - {type: RectTransform, anchor_preset: TopLeft, size_delta: {x: 200, y: 200}}
            - type: MenuList
              items:
                - {name: Resume, label: Resume, role: Primary}
                - Quit Game
        - name: Health
          components:
            - {type: RectTransform, size_delta: {x: 200, y: 16}}
            - {type: StatBar, role: health, max: 50, value: 50}
        - name: Tabs
          components:
            - {type: RectTransform, size_delta: {x: 300, y: 200}}
            - {type: TabView, tabs: [A, B]}
          children:
            - name: PageA
              components: [{type: RectTransform}]
            - name: PageB
              components: [{type: RectTransform}]
        - name: Confirm
          components:
            - {type: RectTransform, size_delta: {x: 300, y: 160}}
            - type: Dialog
              title: Sure?
              buttons: [{name: Yes, label: Yes, role: Success}, {name: No, label: No}]
)";

} // namespace

COOPA_TEST(composites_expand_and_slot_authored_children) {
    Scene scene = load_ui_scene(kCompositeScene);
    SceneObject* hud = scene.find_object("hud");
    SceneObject* inv = scene.find_object("Inventory");
    SceneObject* hello = scene.find_object("Hello");
    expect(hud && inv && hello, "ui: the composite scene loads");
    if (!hud || !inv || !hello) return;
    expect(!inv->children().empty() && inv->children().front()->get_component<coopa::ui::UiGenerated>() != nullptr,
           "ui: a Window's generated frame is its first child, tagged UiGenerated");
    expect(hello->parent() && hello->parent()->name() == "Body", "ui: authored children move into the Window's Body");
    expect(coopa::ui::is_ui_generated(hello->parent()) && !coopa::ui::is_ui_generated(inv),
           "ui: is_ui_generated() tells generated nodes from authored ones");
    expect(inv->find_descendant("Close") && inv->find_descendant("Close")->get_component<coopa::ui::Button>(),
           "ui: close_button builds a Button named Close");

    coopa::ui::UiHandle ui(hud);
    expect(ui.find<coopa::ui::Button>("Resume") && ui.find<coopa::ui::Button>("QuitGame"),
           "ui: MenuList buttons take their item names (a bare label becomes QuitGame)");
    expect(std::abs(ui.get<float>("Volume") - 4.0f) < 1e-4f, "ui: a SettingRow's slider is reachable by the row's name");
    ui.set("Volume", 7.0f);
    expect(std::abs(ui.get<float>("Volume") - 7.0f) < 1e-4f, "ui: set() writes it");
    expect(ui.find<coopa::ui::ProgressBar>("Health") != nullptr, "ui: a StatBar's bar is found by the StatBar's name");

    SceneObject* page_a = scene.find_object("PageA");
    SceneObject* page_b = scene.find_object("PageB");
    expect(page_a && page_b && page_a->parent() && page_b->parent() && page_a->parent()->name() == "Page_A" &&
           page_b->parent()->name() == "Page_B", "ui: a TabView's children fill its pages in order");

    // Layout: the composite's frame stretches to the authored rect.
    auto* canvas = hud->get_component<coopa::ui::CanvasComponent>();
    canvas->rebuild_layout(1280, 720);
    canvas->rebuild_layout(1280, 720);
    const auto* frame_rt = inv->children().front()->get_component<coopa::ui::RectTransform>();
    expect(frame_rt && std::abs(frame_rt->rect().size().x - 400.0f) < 0.5f && std::abs(frame_rt->rect().size().y - 300.0f) < 0.5f,
           "ui: the generated frame fills the Window's 400x300 rect");
    const auto* body_rt = hello->parent()->get_component<coopa::ui::RectTransform>();
    const auto inv_rect = inv->get_component<coopa::ui::RectTransform>()->rect();
    expect(body_rt && body_rt->rect().min.x > inv_rect.min.x && body_rt->rect().max.y < inv_rect.max.y,
           "ui: the Body sits inside the window, below its title bar");
}

COOPA_TEST(handle_binds_signals_bars_and_dialogs_by_name) {
    Scene scene = load_ui_scene(kCompositeScene);
    coopa::ui::UiHandle ui(scene.find_object("hud"));
    int clicks = 0;
    auto c = ui.on_click("Resume", [&] { ++clicks; });
    scene.events().emit("Resume", "click");
    expect(clicks == 1, "ui: on_click() hears the button's named click on the scene EventBus");

    float seen = -1.0f;
    auto c2 = ui.on("Health", "value_changed", [&](const coopa::event::EventArgs& a) { seen = a.get("value", -2.0f); });
    ui.set("Health", 20.0f);
    expect(std::abs(seen - 20.0f) < 1e-4f, "ui: a ProgressBar publishes value_changed by name");

    coopa::stat::Resource hp{80.0f};
    hp.current = 30.0f;
    ui.bind_bar("Health", &hp);
    expect(std::abs(ui.get<float>("Health") - 30.0f) < 1e-4f, "ui: bind_bar() syncs the bar to a Resource");
    hp.damage(10.0f);
    expect(std::abs(ui.get<float>("Health") - 20.0f) < 1e-4f, "ui: ...and follows it");

    // Dialog: a footer button closes it, and "closed" is published by the dialog's name.
    bool closed = false;
    auto c3 = ui.on("Confirm", "closed", [&](const coopa::event::EventArgs&) { closed = true; });
    auto* yes = ui.find<coopa::ui::Button>("Yes");
    expect(yes != nullptr && ui.visible("Confirm"), "ui: a Dialog builds its footer buttons and starts open");
    if (yes) yes->on_click.emit();
    expect(!ui.visible("Confirm") && closed, "ui: a footer button closes the dialog and publishes closed");
    ui.show("Confirm");
    expect(ui.visible("Confirm"), "ui: show() reopens it");
}

COOPA_TEST(composites_take_the_nearest_theme) {
    const std::filesystem::path dir = coopa::test::scratch_dir();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "red.yaml");
        f << "name: red\npanel:\n  panel: { r: 1.0, g: 0.0, b: 0.0, a: 1.0 }\n";
    }
    const std::string yaml = std::string(R"(
scene:
  scene_name: theme_test
  root_objects:
    - name: themed
      components:
        - {type: RectTransform, anchor_preset: StretchAll}
        - {type: Canvas}
        - {type: Theme, source: red.yaml}
      children:
        - name: W
          components:
            - {type: RectTransform, size_delta: {x: 100, y: 100}}
            - {type: Window}
    - name: plain
      components:
        - {type: RectTransform, anchor_preset: StretchAll}
        - {type: Canvas}
      children:
        - name: P
          components:
            - {type: RectTransform, size_delta: {x: 100, y: 100}}
            - {type: ThemedPanel}
)");
    coopa::ui::ThemeLibrary::instance().set_active(coopa::ui::UITheme::builtin_dark());
    Scene scene = load_ui_scene(yaml, (dir / "scene.yaml").string());
    SceneObject* w = scene.find_object("W");
    auto* img = w && !w->children().empty() ? w->children().front()->get_component<coopa::ui::Image>() : nullptr;
    expect(img && img->color.r > 0.99f && img->color.g < 0.01f, "ui: a composite takes its colours from the nearest Theme");
    expect(scene.find_object("themed")->get_component<coopa::ui::ThemeScope>() != nullptr, "ui: a Theme component stays as a ThemeScope");
}

/**
 * @brief The shipped game UI themes (assets/ui/themes: default, Blender, Unity) all load through
 *        the game's own parser and name fonts that exist next to them.
 */
COOPA_TEST(shipped_themes_load_with_their_fonts) {
    const std::filesystem::path dir = std::filesystem::path(ROOT_DIR) / "assets" / "ui" / "themes";
    for (const char* name : {"default", "blender", "unity"}) {
        const std::filesystem::path p = dir / (std::string(name) + ".yaml");
        coopa::ui::UITheme t;
        try {
            t = coopa::ui::load_theme_file(p.string());
        } catch (const std::exception& e) {
            expect(false, std::string("ui themes: ") + name + " loads (" + e.what() + ")");
            continue;
        }
        bool fonts_exist = std::filesystem::exists(dir / t.text.font_path);
        for (const auto* role : {&t.text.title, &t.text.heading, &t.text.body, &t.text.label, &t.text.caption, &t.text.numeric}) {
            if (!role->path.empty()) fonts_exist = fonts_exist && std::filesystem::exists(dir / role->path);
        }
        expect(fonts_exist, std::string("ui themes: ") + name + "'s fonts exist relative to it");
    }
}

COOPA_TEST(placed_canvas_maps_scissor_and_cursor) {
    using coopa::ui::UiPass;
    auto s = UiPass::place_scissor({0, 0, 100, 50}, glm::vec2(200.0f, 100.0f), 1280, 720);
    expect(s.x == 200 && s.y == 100 && s.w == 100 && s.h == 50, "ui: a placed canvas's scissor moves by its origin");
    s = UiPass::place_scissor({0, 0, 300, 50}, glm::vec2(1200.0f, 700.0f), 1280, 720);
    expect(s.x == 1200 && s.w == 80 && s.h == 20, "ui: ...and is clamped to the target");

    Scene scene = load_ui_scene(kCompositeScene);
    auto* canvas = scene.find_object("hud")->get_component<coopa::ui::CanvasComponent>();
    canvas->set_viewport(640, 360);
    canvas->set_screen_origin(glm::vec2(100.0f, 50.0f));
    coopa::input::Input input;
    input.push_cursor_position(100.0 + 320.0, 50.0 + 180.0);
    canvas->set_input(input);
    const glm::vec2 c = canvas->input().position();
    const glm::vec2 half = canvas->root_rect().size() * 0.5f;
    expect(std::abs(c.x - half.x) < 0.5f && std::abs(c.y - half.y) < 0.5f,
           "ui: the cursor maps into a placed canvas (window centre of the rect -> canvas centre)");
    canvas->set_input_enabled(false);
    canvas->set_input(input);
    expect(canvas->input().position().x < -1000.0f, "ui: with input disabled the canvas sees an idle, far-away cursor");

    // A scene that does not simulate still draws its canvases through preview_refresh().
    scene.set_simulating(false);
    canvas->preview_refresh();
    expect(!canvas->draw_list().vertices().empty(), "ui: preview_refresh() lays out and emits a non-simulating canvas");
}

COOPA_TEST(prefab_instance_rect_transform_replaces_the_assets) {
    const std::filesystem::path dir = coopa::test::scratch_dir();
    std::filesystem::create_directories(dir / "ui");
    {
        std::ofstream f(dir / "ui" / "badge.yaml");
        f << "object:\n  name: badge\n  components:\n"
             "    - {type: RectTransform, anchor_preset: TopRight, anchored_position: {x: -10, y: -10}, size_delta: {x: 64, y: 64}}\n"
             "    - {type: ThemedPanel}\n";
    }
    fkyaml::node inst = fkyaml::node::deserialize(std::string(
        "name: b\nprefab: ui/badge\ncomponents:\n  - {type: RectTransform, anchor_preset: BottomLeft, size_delta: {x: 32, y: 32}}\n"));
    const fkyaml::node r = coopa::scene::SceneInheritance::resolve_object(inst, (dir / "scene.yaml").string(),
        [](const std::string& p) { return coopa::yaml::load_document(p); });
    int rects = 0;
    bool kept_panel = false;
    std::string preset;
    for (const auto& c : r.at("components")) {
        const std::string t = c.at("type").get_value<std::string>();
        if (t == "RectTransform") { ++rects; preset = c.contains("anchor_preset") ? c.at("anchor_preset").get_value<std::string>() : ""; }
        if (t == "ThemedPanel") kept_panel = true;
    }
    expect(rects == 1 && preset == "BottomLeft" && !r.at("components").as_seq()[0].contains("anchored_position"),
           "ui: a UI prefab instance's own RectTransform replaces the asset's (placed, not merged)");
    expect(kept_panel, "ui: ...while its other components still come from the asset");
}
