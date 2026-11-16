/**
 * @file quadrant_dock_test.cpp
 * @brief QuadrantDock's component wiring and default-slot-assignment logic.
 *
 * Headless and font-free -- TextRenderer with no Font measures every glyph at 0.55em and
 * draws nothing, so layout and the widgets' own logic are fully exercisable with no GPU, the
 * same property imm_game_test.cpp's screen tests rely on. theme_ref is left empty here
 * deliberately: resolving one requires FontDefaults::search_dirs pointed at a real asset
 * root, which no unit test in this suite wires up; GlassTheme's own loading is covered by
 * glass_theme_test.cpp instead, and the built-in look this exercises is what a screen falls
 * back to regardless.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <coopa/scene/scene.h>
#include <glm/glm.hpp>
#include <coopa/scene/scene_object.h>
#include <toyengine/ui/quadrant_dock.h>
#include <uicoopa/layout/canvas.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("quadrant_dock");

namespace {

/// Builds the canvas and runs preview_refresh() once, exactly as imm_game_test.cpp's screen
/// tests do: start() -> update() -> late_update() is the one whole frame ImmScreen needs to
/// queue and apply its canvas build (see ImmScreen::update()'s own doc for why it cannot
/// happen synchronously).
void prime(coopa::scene::Scene& scene, toy::ui::QuadrantDock* dock) {
    scene.start();
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    require(dock->canvas() != nullptr, "the canvas built after one whole frame");

    coopa::ui::CanvasComponent* canvas =
        dock->canvas()->owner->parent()->get_component<coopa::ui::CanvasComponent>();
    require(canvas != nullptr, "the canvas's CanvasComponent is reachable through its parent");
    canvas->set_viewport(1280, 720);
    canvas->preview_refresh();
}

}  // namespace

COOPA_TEST(three_panels_open_by_default) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->open_panel_count() == 3, "Inventory, Character and Quests are open at start (" +
                                              std::to_string(dock->open_panel_count()) + ")");
}

COOPA_TEST(every_panel_closed_at_start_leaves_nothing_open) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->open_inventory = false;
    dock->open_character = false;
    dock->open_quests = false;
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->open_panel_count() == 0, "nothing is open when every open_* field is false");
}

COOPA_TEST(more_than_four_requested_panels_cap_at_four) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->open_inventory = true;
    dock->open_character = true;
    dock->open_map = true;
    dock->open_quests = true;
    dock->open_settings = true;   // a fifth request; only four quadrants exist
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->open_panel_count() == 4, "at most four panels are ever open at once (" +
                                              std::to_string(dock->open_panel_count()) + ")");
}

COOPA_TEST(drawing_with_an_empty_theme_ref_does_not_crash) {
    // theme_ref is empty by default (the built-in look), and reduced_transparency/
    // reduced_motion both take the draw()/draw_backdrop_()/draw_dock_() branches that skip
    // Context::glass() entirely -- this is the path that must survive with no GPU and no font.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->reduced_transparency = true;
    dock->reduced_motion = true;
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->canvas() != nullptr, "the screen drew without crashing under reduced motion/transparency");
}

COOPA_TEST(the_dock_registers_its_four_panel_icons) {
    // The custom-icon registry is what lets a game add icons uicoopa has no business carrying.
    // Registration happens the first time the dock draws, so prime one first.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    using coopa::ui::imm::Icon;
    for (const char* name : {"Inventory", "Character", "Map", "Quests"}) {
        const Icon ic = coopa::ui::imm::icon_from_name(name);
        expect(ic != Icon::None, std::string(name) + " resolves by name after registration");
        expect(static_cast<int>(ic) >= static_cast<int>(Icon::Count),
               std::string(name) + " is a custom icon, past the built-in set");
        expect(std::string(coopa::ui::imm::icon_name(ic)) == name,
               std::string(name) + " round-trips back to its own name");
    }
    // A built-in must still resolve to itself -- the registry is a fallback, not a replacement.
    expect(coopa::ui::imm::icon_from_name("Gear") == Icon::Gear, "built-in names still win");
}

COOPA_TEST(registering_the_same_icon_name_twice_is_idempotent) {
    using coopa::ui::imm::Icon;
    auto noop = [](coopa::ui::imm::Context&, const coopa::ui::imm::Box&, const glm::vec4&) {};
    const Icon a = coopa::ui::imm::register_custom_icon("QdTestIcon", noop);
    const Icon b = coopa::ui::imm::register_custom_icon("QdTestIcon", noop);
    expect(a == b, "the same name keeps the same Icon value rather than growing the set");
}

COOPA_TEST(home_minimises_and_restores_rather_than_closing_panels) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->reduced_motion = true;   // pin the reveal to its target so one frame settles it
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    const int before = dock->open_panel_count();
    expect(dock->reveal() == 1.0f, "an un-minimised dock is fully revealed");

    dock->minimised = true;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->reveal() == 0.0f, "minimising hides the overlay entirely");
    expect(dock->open_panel_count() == before,
           "minimising does NOT close the panels -- restoring brings the same arrangement back");

    dock->minimised = false;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->reveal() == 1.0f, "restoring reveals the overlay again");
    expect(dock->open_panel_count() == before, "the same panels are still open");
}

COOPA_TEST(the_minimise_transition_reaches_exactly_zero_and_one) {
    // An exponential chase only APPROACHES its target: the overlay crawled through the last
    // tenth of its fade and then cut to nothing when it crossed the draw threshold. A
    // fixed-duration ramp lands on both endpoints exactly, and passes through the middle on
    // the way -- which is what makes the fade read as a fade.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));
    prime(scene, dock);
    expect(dock->reveal() == 1.0f, "starts fully revealed");

    // ~0.22 s end to end, so a few 60 Hz ticks land strictly between the endpoints.
    dock->minimised = true;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    const float mid = dock->reveal();
    expect(mid > 0.0f && mid < 1.0f, "one tick in, the overlay is PARTLY faded (" + std::to_string(mid) + ")");

    for (int i = 0; i < 60; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    expect(dock->reveal() == 0.0f, "it settles on exactly 0, with no residue left to cut off");

    dock->minimised = false;
    for (int i = 0; i < 60; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    expect(dock->reveal() == 1.0f, "and back to exactly 1");
}
