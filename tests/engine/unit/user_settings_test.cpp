/**
 * @file user_settings_test.cpp
 * @brief UserSettings (per-player settings.yaml): unset values fall back, only a change is
 *        written (directories created), a value survives a reload, and the four value types
 *        keep to themselves.
 */

#include <coopa/testing/test.h>

#include <filesystem>

#include <toyengine/core/user_settings.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("user_settings");

COOPA_TEST(values_round_trip_and_only_changes_are_written) {
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir("user_settings");
    const fs::path file = dir / "sub" / "settings.yaml";
    toy::core::UserSettings s;
    s.load(file);
    expect(!s.get_float("audio.music") && s.get_float("audio.music", 0.25f) == 0.25f, "an unset value falls back");
    expect(s.flush() && !fs::exists(file), "nothing changed: nothing written");
    s.set_float("audio.music", 0.5f);
    expect(s.dirty() && s.flush() && fs::exists(file), "a change is written (directories created)");
    toy::core::UserSettings again;
    again.load(file);
    expect_near(again.get_float("audio.music", 0.0f), 0.5f, 1e-6f, "the value survives a reload");
}

COOPA_TEST(every_value_type_round_trips_and_types_do_not_cross) {
    namespace fs = std::filesystem;
    const fs::path file = coopa::test::scratch_dir("user_settings_types") / "settings.yaml";
    toy::core::UserSettings s;
    s.load(file);
    s.set_float("ui.hud_scale", 1.25f);
    s.set_int("ui.hotbar_slots", 7);
    s.set_bool("ui.hover_text", false);
    s.set_string("ui.theme", "habitat");
    expect(s.flush(), "a change of any type is written");

    toy::core::UserSettings again;
    again.load(file);
    expect_near(again.get_float("ui.hud_scale", 0.0f), 1.25f, 1e-6f, "a float survives a reload");
    expect(again.get_int("ui.hotbar_slots", 0) == 7, "an int survives a reload");
    expect(again.get_bool("ui.hover_text", true) == false, "a bool survives a reload");
    expect(again.get_string("ui.theme", "") == "habitat", "a string survives a reload");

    // A key keeps the type it was written with: a reader asking for the wrong one gets its
    // fallback rather than a silent coercion, so a mistyped key cannot quietly mean something.
    expect(!again.get_bool("ui.hud_scale"), "a float does not read as a bool");
    expect(!again.get_string("ui.hud_scale"), "a float does not read as a string");
    expect(!again.get_float("ui.theme"), "a string does not read as a float");

    // erase() is what "reset to defaults" is built from: the key is gone, so the next read
    // falls back to the game's own default rather than to a zero someone wrote over it.
    expect(again.has("ui.hud_scale"), "a written key is present");
    again.erase("ui.hud_scale");
    expect(!again.has("ui.hud_scale"), "an erased key is gone");
    expect_near(again.get_float("ui.hud_scale", 1.0f), 1.0f, 1e-6f, "and reads as the fallback");
}

/**
 * @brief on_changed fires with the key every setter writes, and not when a write is a no-op.
 *
 * This is the hook a host applies a setting through: Engine subscribes once and re-applies the
 * audio bus levels and the interface scale, so a settings slider is audible immediately rather
 * than on the next launch. It has to fire for every type (a toggle writes a bool, a combo a
 * string) and it has to stay quiet when nothing actually changed, or applying a setting would
 * run every frame a panel redraws.
 */
COOPA_TEST(on_changed_reports_every_write_and_stays_quiet_otherwise) {
    toy::core::UserSettings us;
    std::vector<std::string> seen;
    auto conn = us.on_changed.connect_scoped([&](const std::string& k) { seen.push_back(k); });

    us.set_float("audio.master", 0.5f);
    us.set_bool("display.vsync", true);
    us.set_int("ui.hotbar_slots", 8);
    us.set_string("render.quality", "High");
    expect(seen.size() == 4, "one event per write (" + std::to_string(seen.size()) + ")");
    expect(seen[0] == "audio.master" && seen[3] == "render.quality", "each event names its key");

    // Writing the same value is not a change: the setters return early, so a panel redrawing
    // every frame does not re-apply the setting every frame.
    us.set_float("audio.master", 0.5f);
    us.set_bool("display.vsync", true);
    expect(seen.size() == 4, "an unchanged write is silent");

    us.set_float("audio.master", 0.25f);
    expect(seen.size() == 5 && seen.back() == "audio.master", "a real change fires again");

    // erase() is a change too -- "reset to defaults" has to be applied like any other edit.
    us.erase("audio.master");
    expect(seen.size() == 6 && seen.back() == "audio.master", "erase fires");
    us.erase("audio.master");
    expect(seen.size() == 6, "erasing a missing key is silent");
}
