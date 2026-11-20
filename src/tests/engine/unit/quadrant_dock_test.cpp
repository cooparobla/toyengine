/**
 * @file quadrant_dock_test.cpp
 * @brief QuadrantDock's component wiring: how the open_* fields seed each dock LEVEL, how a
 *        level keeps its own arrangement, the minimise transition, and the scene blur it asks
 *        the renderer for.
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

#include <cmath>
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

COOPA_TEST(the_dock_starts_in_the_level_its_first_open_panel_belongs_to) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    // The defaults ask for Inventory first, which lives in the Player group -- so that is where
    // the dock opens, showing the three Player panels and not the World ones also asked for.
    expect(std::string(dock->level_name()) == "Player",
           std::string("the dock starts inside the Player group ('") + dock->level_name() + "')");
    expect(dock->open_panel_count() == 3, "Inventory, Character and Skills are open at start (" +
                                              std::to_string(dock->open_panel_count()) + ")");
}

COOPA_TEST(every_panel_closed_at_start_leaves_the_root_level_empty) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->open_inventory = false;
    dock->open_character = false;
    dock->open_skills = false;
    dock->open_map = false;
    dock->open_quests = false;
    dock->open_settings = false;
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->level() == -1, "with nothing asked for, the dock opens at the root level");
    expect(dock->open_panel_count() == 0, "nothing is open when every open_* field is false");
}

COOPA_TEST(more_than_four_panels_in_one_level_cap_at_four) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    // Every one of them: the Player group has four panels and four quadrants, so its level
    // fills exactly, and the three groups that follow are seeded independently rather than
    // competing for the same four slots.
    dock->open_inventory = true;
    dock->open_character = true;
    dock->open_skills = true;
    dock->open_equipment = true;
    dock->open_map = true;
    dock->open_quests = true;
    dock->open_bestiary = true;
    dock->open_settings = true;
    dock->open_audio = true;
    dock->open_controls = true;
    dock->open_journal = true;
    scene.add_root_object(std::move(obj));

    prime(scene, dock);
    expect(dock->open_panel_count() == 4, "at most four panels are ever open at once (" +
                                              std::to_string(dock->open_panel_count()) + ")");
}

COOPA_TEST(each_level_keeps_its_own_arrangement) {
    // The whole point of a group: it is a place with its own workspace, which it still has when
    // you come back to it. show_level() is what the dock's group and Back buttons call.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->reduced_motion = true;
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    const int player_panels = dock->open_panel_count();
    expect(player_panels == 3, "the Player group starts with three panels");

    dock->show_level(1);   // World: seeded with Map and Quests by the defaults
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(std::string(dock->level_name()) == "World", "show_level(1) enters the World group");
    expect(dock->open_panel_count() == 2,
           "World shows the two panels the defaults seeded it with (" +
               std::to_string(dock->open_panel_count()) + ")");

    dock->show_level(-1);  // the root level, which nothing seeded
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->level() == -1 && dock->open_panel_count() == 0,
           "the root level is its own empty workspace, not the group's");

    dock->show_level(0);
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->open_panel_count() == player_panels,
           "coming back to Player restores exactly what it had (" +
               std::to_string(dock->open_panel_count()) + ")");
}

COOPA_TEST(show_level_ignores_a_level_that_does_not_exist) {
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    const int before = dock->level();
    dock->show_level(7);
    dock->show_level(-5);
    expect(dock->level() == before, "an out-of-range group leaves the dock where it was");
}

COOPA_TEST(the_overlay_asks_for_a_scene_blur_and_stops_when_minimised) {
    // The UI half of ToyRenderConfig::ui_scene_blur: Engine reads ImmScreen::scene_blur() after
    // late_update() and hands the largest to the renderer, so what this asserts is exactly what
    // reaches the frame. reduced_motion settles the reveal in one tick.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    dock->reduced_motion = true;
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    expect(dock->scene_blur() == 1.0f, "a revealed overlay asks for the world to be fully blurred (" +
                                           std::to_string(dock->scene_blur()) + ")");

    dock->minimised = true;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->scene_blur() == 0.0f, "minimising hands the sharp frame straight back");

    // Opaque panes leave nothing of the world visible, so the fade would buy an invisible
    // effect -- the dock does not ask for it at all.
    dock->minimised = false;
    dock->reduced_transparency = true;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    expect(dock->scene_blur() == 0.0f, "reduced transparency asks for no blur");
}

COOPA_TEST(the_blur_fades_with_the_overlay_rather_than_cutting) {
    // Without reduced_motion the request has to track the reveal through the middle of the
    // transition: a blur that snapped on would be a different effect from the one the overlay
    // is doing beside it.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    dock->minimised = true;
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    const float mid = dock->scene_blur();
    expect(mid > 0.0f && mid < 1.0f, "one tick into minimising, the blur is PARTLY faded (" +
                                         std::to_string(mid) + ")");
    expect(std::abs(mid - dock->reveal()) < 1e-5f, "it is the reveal itself, not a second curve");
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

COOPA_TEST(the_dock_registers_its_own_panel_icons) {
    // The custom-icon registry is what lets a game add icons uicoopa has no business carrying.
    // Registration happens the first time the dock draws, so prime one first.
    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Dock");
    toy::ui::QuadrantDock* dock = obj->add_component<toy::ui::QuadrantDock>();
    scene.add_root_object(std::move(obj));
    prime(scene, dock);

    using coopa::ui::imm::Icon;
    for (const char* name : {"Inventory", "Character", "Skills", "Equipment", "Map", "Quests",
                             "Bestiary", "Journal", "Audio", "Sliders", "PlayerGroup"}) {
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
