/**
 * @file user_settings_test.cpp
 * @brief UserSettings (per-player settings.yaml): unset values fall back, only a change is
 *        written (directories created), and a value survives a reload.
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
