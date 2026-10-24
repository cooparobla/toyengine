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
