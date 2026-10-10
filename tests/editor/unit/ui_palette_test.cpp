/**
 * @file ui_palette_test.cpp
 * @brief The UI designer's widget palette: every entry loads under a canvas through the engine's scene
 * loader and its main component has an inspector schema.
 */

#include <coopa/testing/test.h>

#include <coopa/scene/scene_loader.h>
#include <uicoopa/ui_yaml.h>

#include "editor/schema/component_schema.h"
#include "editor/support/fixtures.h"
#include "editor/ui/ui_palette.h"

COOPA_TEST_SUITE("ui_palette");

namespace toy::editor::testing {

COOPA_TEST(every_palette_entry_loads_under_a_canvas) {
    coopa::ui::register_ui_components();
    int loaded = 0;
    for (const auto& e : ui::palette()) {
        Node made = e.make();
        Node obj;
        if (e.component) {
            obj = ui::palette_detail::object("Host", {ui::palette_detail::centered(100, 100), made});
        } else {
            obj = made;
        }
        Node canvas = ui::palette_detail::object("Canvas", {ui::palette_detail::stretch(), ui::palette_detail::comp("Canvas")}, {obj});
        Node doc = Node::mapping();
        Node sc = Node::mapping();
        sc["scene_name"] = Node(std::string("PaletteProbe"));
        Node roots = Node::sequence();
        roots.as_seq().push_back(canvas);
        sc["root_objects"] = roots;
        doc["scene"] = sc;
        try {
            coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(doc, "palette_probe.yaml");
            auto* c = scene.find_object("Canvas");
            auto* canvas_comp = c ? c->get_component<coopa::ui::CanvasComponent>() : nullptr;
            expect(canvas_comp != nullptr && c->children().size() == 1, "palette " + e.id + ": loads under a canvas");
            if (canvas_comp) {
                canvas_comp->set_viewport(1920, 1080);
                canvas_comp->preview_refresh();
                expect(!canvas_comp->draw_list().vertices().empty() || e.id == "container" || e.id == "spacer" ||
                       e.id == "vstack" || e.id == "hstack" || e.id == "grid" || e.id == "hud_corner" || e.id == "message_log" ||
                       e.id == "label" || e.id == "text" || e.id == "title" || e.id == "prompt_bar" || e.component,   // text / icons only: no font headless
                       "palette " + e.id + ": draws something");
            }
            ++loaded;
        } catch (const std::exception& ex) {
            expect(false, "palette " + e.id + " throws: " + ex.what());
        }
        expect(find_schema(component_type(e.component ? made : made.at("components").as_seq().back())) != nullptr,
               "palette " + e.id + ": its main component has an inspector schema");
    }
    expect(loaded >= 30, "every palette entry loads (" + std::to_string(loaded) + ")");
}

} // namespace toy::editor::testing
